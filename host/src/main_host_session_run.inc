    HostSession(remotex::core::AuthClient* auth,
                const std::string& machine_id,
                const std::string& machine_key)
        : auth_(auth), machine_id_(machine_id), machine_key_(machine_key) {}

    ~HostSession() { Stop(); }

    static void JoinOwnedThread(std::thread& thread, const char* label) {
        if (!thread.joinable()) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            Log("WARNING: %s attempted to join itself; detaching instead", label);
            thread.detach();
            return;
        }
        thread.join();
    }

    void StartCaptureCadenceWorker() {
        std::thread stale_thread;
        {
            std::lock_guard lock(capture_cadence_thread_mutex_);
            if (capture_cadence_thread_.joinable() && !capture_cadence_running_.load()) {
                stale_thread = std::move(capture_cadence_thread_);
            }
        }
        JoinOwnedThread(stale_thread, "capture cadence worker restart");

        std::lock_guard lock(capture_cadence_thread_mutex_);
        if (capture_cadence_running_.exchange(true)) {
            return;
        }
        if (capture_cadence_thread_.joinable()) {
            Log("WARNING: capture cadence worker already owns a thread handle during restart; skipping duplicate start");
            return;
        }
        capture_cadence_thread_ = std::thread([this]() { CaptureCadenceLoop(); });
    }

    void StopCaptureCadenceWorker() {
        capture_cadence_running_.store(false);

        std::thread owned_thread;
        {
            std::lock_guard lock(capture_cadence_thread_mutex_);
            if (capture_cadence_thread_.joinable()) {
                owned_thread = std::move(capture_cadence_thread_);
            }
        }
        JoinOwnedThread(owned_thread, "capture cadence worker stop");

        std::lock_guard lock(capture_state_mutex_);
        latest_prepared_frame_.reset();
    }

    void CaptureCadenceLoop() {
        while (capture_cadence_running_.load()) {
            if (!peer_connected_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (capture_reinit_pending_.exchange(false)) {
                const auto desktop_before_reinit = remotex::host::CaptureDesktopContext();
                RecordSecureTransitionStage(
                    "capture_reinit_started",
                    std::string("input=") +
                        (desktop_before_reinit.input_desktop_name.empty()
                            ? "<unknown>"
                            : desktop_before_reinit.input_desktop_name) +
                        " thread=" +
                        (desktop_before_reinit.thread_desktop_name.empty()
                            ? "<unknown>"
                            : desktop_before_reinit.thread_desktop_name),
                    false,
                    true);
                if (desktop_before_reinit.HasCompleteContext() &&
                    !desktop_before_reinit.InputMatchesThread()) {
                    DWORD switch_error = ERROR_SUCCESS;
                    remotex::host::DesktopContextSnapshot desktop_after_switch;
                    if (remotex::host::SwitchCurrentThreadToInputDesktop(
                            switch_error,
                            &desktop_after_switch)) {
                        RecordSecureTransitionStage(
                            "capture_thread_rebound",
                            std::string("input=") +
                                (desktop_after_switch.input_desktop_name.empty()
                                    ? "<unknown>"
                                    : desktop_after_switch.input_desktop_name) +
                                " thread=" +
                                (desktop_after_switch.thread_desktop_name.empty()
                                    ? "<unknown>"
                                    : desktop_after_switch.thread_desktop_name),
                            false,
                            true);
                        Log("Capture thread switched to input desktop before reinit: input='%s' thread='%s'",
                            desktop_after_switch.input_desktop_name.empty()
                                ? "<unknown>"
                                : desktop_after_switch.input_desktop_name.c_str(),
                            desktop_after_switch.thread_desktop_name.empty()
                                ? "<unknown>"
                                : desktop_after_switch.thread_desktop_name.c_str());
                    } else {
                        RecordSecureTransitionStage(
                            "capture_thread_rebind_failed",
                            std::string("input=") +
                                (desktop_before_reinit.input_desktop_name.empty()
                                    ? "<unknown>"
                                    : desktop_before_reinit.input_desktop_name) +
                                " thread=" +
                                (desktop_before_reinit.thread_desktop_name.empty()
                                    ? "<unknown>"
                                    : desktop_before_reinit.thread_desktop_name) +
                                " err=" + std::to_string(switch_error),
                            true);
                        Log("ERROR: Failed to switch capture thread to input desktop before reinit: input='%s' thread='%s' err=%lu",
                            desktop_before_reinit.input_desktop_name.empty()
                                ? "<unknown>"
                                : desktop_before_reinit.input_desktop_name.c_str(),
                            desktop_before_reinit.thread_desktop_name.empty()
                                ? "<unknown>"
                                : desktop_before_reinit.thread_desktop_name.c_str(),
                            static_cast<unsigned long>(switch_error));
                        Log("Capture cadence worker will restart on a fresh thread before retrying secure-desktop recovery");
                        capture_.Shutdown();
                        {
                            std::lock_guard lock(capture_state_mutex_);
                            ResetCaptureFrameStateLocked();
                            last_frame_sent_time_ = std::chrono::steady_clock::now();
                            last_successful_capture_time_ = last_frame_sent_time_;
                        }
                        capture_reinit_pending_.store(true);
                        capture_cadence_restart_requested_.store(true);
                        capture_cadence_running_.store(false);
                        break;
                    }
                }

                Log("Reinitializing screen capture for %s session",
                    IsWatchOnlySession() ? "watch-only" : "interactive");
                if (!capture_.Reinitialize()) {
                    RecordSecureTransitionStage("capture_reinit_failed", "capture_.Reinitialize returned false", true);
                    Log("ERROR: Screen capture reinitialize failed during pending recovery");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                {
                    std::lock_guard lock(capture_state_mutex_);
                    ResetCaptureFrameStateLocked();
                    last_frame_sent_time_ = std::chrono::steady_clock::now();
                    last_successful_capture_time_ = last_frame_sent_time_;
                }
                RecordSecureTransitionStage(
                    "capture_reinit_succeeded",
                    std::string("session=") +
                        (IsWatchOnlySession() ? "watch-only" : "interactive"),
                    false,
                    true);
                const auto& od = capture_.GetOutputDesc();
                injector_.SetCapturedMonitorBounds(
                    od.DesktopCoordinates.left,
                    od.DesktopCoordinates.top,
                    od.DesktopCoordinates.right,
                    od.DesktopCoordinates.bottom);
                peer_.CleanupVideoEncoder();
            }

            const auto capture_start = std::chrono::steady_clock::now();
            remotex::host::CapturedFrame frame;
            const auto capture_result = capture_.CaptureFrame(frame, 16);
            const auto capture_done = std::chrono::steady_clock::now();
            ProcessCaptureAttempt(frame, capture_result, capture_start, capture_done);
            if (capture_cadence_restart_requested_.exchange(false)) {
                Log("Restarting capture cadence worker on a fresh thread after secure-desktop transition failure");
                capture_reinit_pending_.store(true);
                capture_cadence_running_.store(false);
                break;
            }
        }
    }

    void Run() {
        // â”€â”€ TURN credentials â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        remotex::core::TurnClientConfig turn_cfg;
        turn_cfg.api_host = g_connection_settings.api_host;
        turn_cfg.api_port = g_connection_settings.api_port;
        turn_cfg.use_tls  = g_connection_settings.api_use_tls;
        turn_client_.Configure(turn_cfg, auth_);

        remotex::core::MachineClientConfig machine_cfg;
        machine_cfg.api_host = g_connection_settings.api_host;
        machine_cfg.api_port = g_connection_settings.api_port;
        machine_cfg.use_tls  = g_connection_settings.api_use_tls;
        machine_client_.Configure(machine_cfg, auth_);

        remotex::core::TurnCredentials turn_creds;
        if (turn_client_.FetchCredentials(auth_->GetUserID(), turn_creds)) {
            ice_servers_ = remotex::core::TurnClient::CredentialsToIceServers(turn_creds);
            Log("TURN credentials obtained (%zu servers)", ice_servers_.size());
        } else {
            Log("TURN credentials unavailable, using STUN only");
            remotex::core::IceServer stun;
            stun.urls.push_back("stun:stun.l.google.com:19302");
            ice_servers_.push_back(stun);
        }

        // â”€â”€ Screen capture â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (!capture_.Init()) {
            Log("ERROR: Screen capture init failed (no GPU / access denied)");
            Log("Continuing without capture â€” will retry on session start");
        } else {
            Log("Screen capture ready: %ux%u", capture_.GetWidth(), capture_.GetHeight());
        }

        // â”€â”€ Input injector â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        injector_.Start();
        secure_desktop_input_bridge_ = std::make_unique<SecureDesktopInputHelperBridge>(GetCurrentExecutablePath());
        injector_.SetSecureDesktopBridge(secure_desktop_input_bridge_.get());
        {
            const auto& od = capture_.GetOutputDesc();
            injector_.SetCapturedMonitorBounds(
                od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                od.DesktopCoordinates.right, od.DesktopCoordinates.bottom);
        }
        Log("Input injector started");

        // â”€â”€ Clipboard sync â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        clipboard_sync_.SetOnClipboardChanged([this](const remotex::host::ClipboardSync::ClipboardPayload& payload) {
            if (!peer_connected_ || IsWatchOnlySession()) {
                return;
            }

            if (payload.IsFiles()) {
                CancelClipboardFileOffers("host file clipboard superseded the previous session clipboard offer");

                ClipboardFileOffer offer;
                offer.token = GenerateClipboardTransferToken();
                offer.source_paths = payload.file_paths;
                offer.entry_names = TopLevelClipboardEntryNames(payload.file_paths);
                if (offer.entry_names.empty()) {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(clipboard_file_state_mutex_);
                    pending_outgoing_clipboard_offers_[offer.token] = offer;
                }

                json control_payload;
                control_payload["type"] = "clipboard_files_offer";
                control_payload["token"] = offer.token;
                control_payload["entry_names"] = json::array();
                for (const auto& entry_name : offer.entry_names) {
                    control_payload["entry_names"].push_back(entry_name);
                }
                peer_.SendDataChannelMessage("control", control_payload.dump());
                Log("Clipboard files announced from host token=%s entries=%zu",
                    offer.token.c_str(),
                    offer.entry_names.size());
                return;
            }

            CancelClipboardFileOffers("host text clipboard superseded pending file clipboard transfer");
            peer_.SendDataChannelMessage("clipboard", payload.text);
        });
        clipboard_sync_.Start();
        Log("Clipboard sync started");

        // â”€â”€ Chat window â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        chat_window_.SetSendCallback([this](const std::string& text) {
            if (peer_connected_) {
                // Enviar via data channel P2P (sessÃ£o ativa).
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::string json = "{\"from\":\"host\",\"text\":\"";
                for (char c : text) {
                    switch (c) {
                        case '"':  json += "\\\""; break;
                        case '\\': json += "\\\\"; break;
                        case '\n': json += "\\n"; break;
                        case '\r': json += "\\r"; break;
                        default:   json += c; break;
                    }
                }
                json += "\",\"ts\":" + std::to_string(now) + "}";
                peer_.SendDataChannelMessage("chat", json);
            }
            // Sempre enviar tambÃ©m via signaling (para chat sem sessÃ£o remota).
            if (signaling_.GetState() == remotex::core::SignalingState::Connected) {
                signaling_.SendChatMessage(machine_id_, "host", text);
            }
        });
        chat_window_.Start();
        Log("Chat window started");

        // â”€â”€ Recording uploader â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            remotex::host::RecordingUploader::Config rec_upload_cfg;
            rec_upload_cfg.api_host   = g_connection_settings.api_host;
            rec_upload_cfg.api_port   = g_connection_settings.api_port;
            rec_upload_cfg.use_tls    = g_connection_settings.api_use_tls;
            rec_upload_cfg.machine_id = machine_id_;
            recording_uploader_.Initialize(rec_upload_cfg, auth_);
        }
        Log("Recording uploader initialized");
#ifdef REMOTEX_HAS_FFMPEG
        Log("Session recording support enabled (FFmpeg build detected)");
#else
        Log("WARNING: Session recording support disabled in this build. Set FFMPEG_ROOT before building the host.");
#endif

        // â”€â”€ Signaling â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        ConnectSignaling();

        // â”€â”€ Main loop â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        Log("Host ready â€” waiting for viewer connections...");

        const auto capture_interval = std::chrono::milliseconds(1000 / kCaptureFPS);
        auto next_capture_tick = std::chrono::steady_clock::now();

        while (IsRunning()) {
            static constexpr auto kSignalingLeaseRefresh = std::chrono::seconds(10);
            const auto now = std::chrono::steady_clock::now();
            if (signaling_.GetState() == remotex::core::SignalingState::Connected &&
                now >= next_signaling_refresh_) {
                SaveWorkerSignalingState(true);
                next_signaling_refresh_ = now + kSignalingLeaseRefresh;
            }

            if (peer_connected_) {
                if (!capture_cadence_running_.load()) {
                    StartCaptureCadenceWorker();
                }
                if (send_monitor_list_pending_.exchange(false)) {
                    SendMonitorList();
                    if (SendInitialMonitorStatus()) {
                        initial_monitor_status_sent_.store(true);
                        RearmVideoBootstrapAfterDisplayContract("initial_display_contract");
                    } else {
                        send_monitor_list_pending_.store(true);
                    }
                }
                ProcessMonitorSwitch();
                ProcessDeferredVideoEncoderCleanup();
                if (initial_monitor_status_sent_.load()) {
                    SendCaptureFrame();
                }
                MaybeLogCapturePerf();
            }
            next_capture_tick += capture_interval;
            const auto sleep_started = std::chrono::steady_clock::now();
            if (next_capture_tick > sleep_started) {
                std::this_thread::sleep_until(next_capture_tick);
            } else if (sleep_started - next_capture_tick > capture_interval) {
                // Reset cadence after long work so capture doesn't drift
                // into a permanently lower effective FPS.
                next_capture_tick = sleep_started;
            }
        }

        Stop();
    }

private:
    static std::string NormalizeAccessMode(const std::string& access_mode) {
        return access_mode == std::string(remotex::core::kSessionAccessModeWatchOnly)
            ? std::string(remotex::core::kSessionAccessModeWatchOnly)
            : std::string(remotex::core::kSessionAccessModeInteractive);
    }

    bool IsWatchOnlySession() const {
        return session_access_mode_ == remotex::core::kSessionAccessModeWatchOnly;
    }

    remotex::host::RecordingCanvas CurrentRecordingCanvas() const {
        if (recording_canvas_.width > 0 && recording_canvas_.height > 0) {
            return recording_canvas_;
        }

        auto canvas = remotex::host::ComputeRecordingCanvas(
            std::span<const remotex::host::MonitorInfo>{},
            capture_.GetWidth(),
            capture_.GetHeight());
        if (canvas.width > 0 && canvas.height > 0) {
            return canvas;
        }

        if (available_monitors_.empty()) {
            const auto monitors = remotex::host::ScreenCapture::EnumerateMonitors();
            return remotex::host::ComputeRecordingCanvas(
                monitors,
                0,
                0);
        }

        return remotex::host::ComputeRecordingCanvas(
            available_monitors_,
            0,
            0);
    }

    remotex::host::RecorderConfig BuildRecorderConfig() const {
        remotex::host::RecorderConfig rec_cfg;
        std::error_code temp_dir_error;
        const auto temp_dir = fs::temp_directory_path(temp_dir_error);
        if (temp_dir_error) {
            Log("ERROR: Session recording temp root unavailable: %s",
                temp_dir_error.message().c_str());
            return rec_cfg;
        }
        rec_cfg.output_dir = temp_dir / "remotex_recordings";
        const auto canvas = CurrentRecordingCanvas();
        rec_cfg.width = canvas.width;
        rec_cfg.height = canvas.height;
        rec_cfg.fps = 15;
        rec_cfg.bitrate = 2000000;
        rec_cfg.use_hw_encoder = true;
        return rec_cfg;
    }

    enum class ConsentPromptResult {
        Approved,
        Denied,
        TimedOut,
        Failed,
    };

    static DWORD CurrentProcessSessionId() {
        DWORD session_id = 0xFFFFFFFF;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
            return session_id;
        }
        return WTSGetActiveConsoleSessionId();
    }

    static std::string RequesterDisplayName(const std::string& requester_name) {
        return requester_name.empty() ? "Um operador" : requester_name;
    }

    void SendViewerError(const std::string& message) {
        Log("Sending viewer error: %s", message.c_str());
        signaling_.SendError(machine_id_, message);
    }

    ConsentPromptResult RequestClientConsent(const std::string& requester_name) {
        const DWORD session_id = CurrentProcessSessionId();
        if (session_id == 0xFFFFFFFF) {
            Log("Consent prompt failed: no active desktop session");
            return ConsentPromptResult::Failed;
        }

        const std::string requester = RequesterDisplayName(requester_name);
        const std::wstring title = L"RemoteX";
        const std::wstring text = ToWide(requester + " quer acessar sua maquina.\nPermitir?");
        DWORD response = IDNO;
        const BOOL ok = WTSSendMessageW(
            WTS_CURRENT_SERVER_HANDLE,
            session_id,
            const_cast<LPWSTR>(title.c_str()),
            static_cast<DWORD>((title.size() + 1) * sizeof(wchar_t)),
            const_cast<LPWSTR>(text.c_str()),
            static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)),
            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND,
            30,
            &response,
            TRUE);
        if (!ok) {
            Log("Consent prompt failed: WTSSendMessageW err=%lu", GetLastError());
            return ConsentPromptResult::Failed;
        }
        if (response == IDYES) {
            return ConsentPromptResult::Approved;
        }
        if (response == IDTIMEOUT) {
            return ConsentPromptResult::TimedOut;
        }
        return ConsentPromptResult::Denied;
    }

    bool EnsureInteractiveSessionApproved() {
        if (session_id_from_viewer_.empty()) {
            SendViewerError("Falha ao validar autorizacao do cliente.");
            return false;
        }

        Log("Consent: fetching session %s",
            remotex::core::RedactIdentifier(session_id_from_viewer_, "session").c_str());
        remotex::core::SessionInfo session;
        std::string session_error;
        if (!FetchViewerSession(&session, &session_error)) {
            Log("Consent: GetSession failed for %s: %s",
                remotex::core::RedactIdentifier(session_id_from_viewer_, "session").c_str(),
                session_error.c_str());
            const std::string error = session_error.empty()
                ? "Falha ao validar autorizacao do cliente."
                : session_error;
            SendViewerError(error);
            return false;
        }
        Log("Consent: session status='%s', requester='%s'",
            session.status.c_str(), session.requester_name.c_str());
        if (session.status == std::string(remotex::core::kSessionStatusActive)) {
            return true;
        }
        if (session.status == std::string(remotex::core::kSessionStatusDenied)) {
            SendViewerError("Acesso recusado pelo usuario local.");
            return false;
        }
        if (session.status == std::string(remotex::core::kSessionStatusExpired)) {
            SendViewerError("Tempo limite aguardando autorizacao do usuario local.");
            return false;
        }
        if (session.status != std::string(remotex::core::kSessionStatusPendingConsent)) {
            SendViewerError("Sessao encerrada antes da autorizacao do cliente.");
            return false;
        }

        const ConsentPromptResult prompt_result = RequestClientConsent(session.requester_name);
        std::string decision;
        std::string viewer_error;
        switch (prompt_result) {
            case ConsentPromptResult::Approved:
                decision = std::string(remotex::core::kSessionConsentApproved);
                break;
            case ConsentPromptResult::Denied:
                decision = std::string(remotex::core::kSessionConsentDenied);
                viewer_error = "Acesso recusado pelo usuario local.";
                break;
            case ConsentPromptResult::TimedOut:
                decision = std::string(remotex::core::kSessionConsentExpired);
                viewer_error = "Tempo limite aguardando autorizacao do usuario local.";
                break;
            case ConsentPromptResult::Failed:
            default:
                SendViewerError("Falha ao solicitar autorizacao do usuario local.");
                return false;
        }

        Log("Consent: resolving session %s with decision='%s'",
            remotex::core::RedactIdentifier(session_id_from_viewer_, "session").c_str(),
            decision.c_str());
        auto consent_result = machine_client_.ResolveSessionConsent(
            session_id_from_viewer_, decision, machine_key_);
        Log("Consent: ResolveSessionConsent returned http=%d, has_session=%s, error='%s'",
            consent_result.http_code,
            consent_result.session ? "yes" : "no",
            consent_result.error.c_str());
        if (!consent_result.session) {
            const std::string error = consent_result.error.empty()
                ? "Falha ao registrar autorizacao do cliente."
                : consent_result.error;
            SendViewerError(error);
            return false;
        }
        CacheRelayBundle(*consent_result.session, nullptr);

        if (decision == std::string(remotex::core::kSessionConsentApproved) &&
            consent_result.session->status == std::string(remotex::core::kSessionStatusActive)) {
            return true;
        }

        if (viewer_error.empty()) {
            viewer_error = "Acesso nao autorizado pelo usuario local.";
        }
        SendViewerError(viewer_error);
        return false;
    }
