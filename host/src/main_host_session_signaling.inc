    template <typename Fn>
    void RunTeardownStep(const char* step_name, Fn&& fn) {
        const char* label = step_name ? step_name : "<unknown>";
        Log("Session teardown step: %s", label);
        try {
            fn();
        } catch (const std::exception& e) {
            Log("ERROR: Session teardown step '%s' threw: %s", label, e.what());
        } catch (...) {
            Log("ERROR: Session teardown step '%s' threw an unknown exception", label);
        }
    }

    void Stop() {
        RunTeardownStep("stop_capture_cadence_worker", [this]() {
            StopCaptureCadenceWorker();
        });
        RunTeardownStep("consent_thread_join", [this]() {
            JoinOwnedThread(consent_thread_, "consent worker stop");
        });
        if (!ApplyBlockInputState(false, "session_stop") && input_blocked_.load()) {
            Log("WARNING: Host still believes local input is blocked during session stop");
        }

        peer_connected_ = false;
        peer_session_active_.store(false);
        input_blocked_.store(false);

        RunTeardownStep("finalize_recording", [this]() {
            FinalizeRecordingAndUploadIfPresent("session_stop");
        });
        RunTeardownStep("cancel_clipboard_file_offers", [this]() {
            CancelClipboardFileOffers("host session stop");
        });
        RunTeardownStep("disable_privacy_screen", [this]() {
            privacy_screen_.Disable();
        });
        RunTeardownStep("stop_chat_window", [this]() {
            chat_window_.Stop();
        });
        RunTeardownStep("stop_clipboard_sync", [this]() {
            clipboard_sync_.Stop();
        });
        RunTeardownStep("clear_secure_desktop_bridge", [this]() {
            injector_.SetSecureDesktopBridge(nullptr);
            secure_desktop_input_bridge_.reset();
        });
        RunTeardownStep("stop_input_injector", [this]() {
            injector_.Stop();
        });
        RunTeardownStep("shutdown_file_transfer", [this]() {
            file_transfer_.Shutdown();
        });
        RunTeardownStep("shutdown_peer", [this]() {
            peer_.Shutdown();
        });
        RunTeardownStep("disconnect_signaling", [this]() {
            signaling_.Disconnect();
        });
        RunTeardownStep("shutdown_capture", [this]() {
            capture_.Shutdown();
        });
    }

    void FinalizeRecordingAndUploadIfPresent(const char* reason) {
        const char* safe_reason = reason ? reason : "<unknown>";
        const bool had_recording = recorder_.IsRecording();
        const std::string session_id = session_id_from_viewer_;

        try {
            const auto finalized = remotex::host::FinalizeRecordingIfPresent(
                had_recording,
                recording_start_time_,
                std::chrono::steady_clock::now(),
                session_id,
                [this]() {
                    return recorder_.Stop();
                },
                [this](const fs::path& mp4_path, const std::string& upload_session_id, int duration_sec) {
                    recording_uploader_.UploadAsync(mp4_path, upload_session_id, duration_sec);
                });

            if (had_recording && finalized.path.empty()) {
                Log("Session recording finished without a saved MP4 (reason=%s)", safe_reason);
            } else if (!finalized.path.empty()) {
                Log("Session recording saved: %s (%ds) reason=%s session_id=%s",
                    remotex::core::RedactPath(finalized.path, "recording_file").c_str(),
                    finalized.duration_sec,
                    safe_reason,
                    remotex::core::RedactIdentifier(session_id.empty() ? std::string("<empty>") : session_id,
                                                    "session").c_str());
                if (session_id.empty()) {
                    Log("WARNING: Session recording upload has no backend session id; audit association may be unavailable");
                }
            }
        } catch (const std::exception& e) {
            Log("ERROR: Session recording finalize/upload threw (reason=%s): %s", safe_reason, e.what());
        } catch (...) {
            Log("ERROR: Session recording finalize/upload threw an unknown exception (reason=%s)", safe_reason);
        }

        recording_canvas_ = {};
        recording_start_time_ = std::chrono::steady_clock::time_point{};
    }

    void ResetPeerSessionState(const char* reason) {
        const char* safe_reason = reason ? reason : "<unknown>";
        Log("Resetting WebRTC peer session: %s", safe_reason);

        RunTeardownStep("stop_capture_cadence_worker", [this]() {
            StopCaptureCadenceWorker();
        });
        if (!ApplyBlockInputState(false, safe_reason) && input_blocked_.load()) {
            Log("WARNING: Host still believes local input is blocked after peer reset (%s)", safe_reason);
        }

        peer_connected_ = false;
        peer_session_active_.store(false);
        input_blocked_.store(false);
        first_frame_sent_ = false;
        last_frame_sent_time_ = std::chrono::steady_clock::now();
        last_successful_capture_time_ = std::chrono::steady_clock::now();
        data_channels_configured_ = false;
        session_access_mode_ = std::string(remotex::core::kSessionAccessModeInteractive);

        RunTeardownStep("finalize_recording", [this, safe_reason]() {
            FinalizeRecordingAndUploadIfPresent(safe_reason);
        });
        RunTeardownStep("cancel_clipboard_file_offers", [this, safe_reason]() {
            CancelClipboardFileOffers(std::string("host peer reset: ") + safe_reason);
        });
        RunTeardownStep("disable_privacy_screen", [this]() {
            privacy_screen_.Disable();
        });
        RunTeardownStep("shutdown_file_transfer", [this]() {
            file_transfer_.Shutdown();
        });
        RunTeardownStep("shutdown_peer", [this]() {
            peer_.Shutdown();
        });

        relay_protocol_version_ = 0;
        relay_session_id_.clear();
        relay_role_.clear();
        relay_ticket_.clear();
        relay_ticket_expires_unix_ = 0;
        relay_host_override_.clear();
        relay_port_override_ = 0;
        session_id_from_viewer_.clear();
    }

    void ConnectSignaling() {
        signaling_.SetListener(this);

        remotex::core::SignalingConfig cfg;
        cfg.host       = g_connection_settings.signaling_host;
        cfg.port       = g_connection_settings.signaling_port;
        cfg.path       = "/ws";
        cfg.token_provider = [this](bool force_refresh) {
            if (force_refresh) auth_->RefreshToken();
            return auth_->GetAccessToken();
        };
        cfg.client_id  = machine_id_;
        cfg.use_tls    = g_connection_settings.signaling_use_tls;
        cfg.max_reconnect_attempts = 0;
        cfg.initial_backoff_ms     = 500;
        cfg.max_backoff_ms         = 30000;

        if (signaling_.Connect(cfg)) {
            Log("Signaling connecting to host=%s port=%u...",
                remotex::core::RedactIdentifier(cfg.host, "signaling_host").c_str(),
                cfg.port);
        } else {
            Log("ERROR: Signaling connect failed");
        }
    }

    // â”€â”€ SignalingListener â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void OnConnected() override {
        Log("Signaling connected");
        SaveWorkerSignalingState(true);
        signaling_.JoinSession(machine_id_, remotex::core::signal_role::Host);
        Log("Joined session %s as host",
            remotex::core::RedactIdentifier(machine_id_, "machine").c_str());
    }

    void OnDisconnected() override {
        Log("Signaling disconnected");
        SaveWorkerSignalingState(false);
    }

    void OnSocketActivity() override {
        SaveWorkerSignalingState(true);
    }

    void OnError(const std::string& error) override {
        Log("Signaling error: %s", error.c_str());
    }

    void OnSignalingMessage(const remotex::core::SignalingEvent& event) override {
        using namespace remotex::core;

        if (event.type == signal_msg::PeerJoined) {
            if (peer_session_active_.load()) {
                if (peer_connected_) {
                    Log("Viewer joined while peer already connected - ignoring duplicate join");
                    return;
                }

                Log("Viewer joined while stale peer session was still active - rebuilding peer");
                ResetPeerSessionState("viewer_rejoin_stale");
            }
            Log("Viewer joined session");
        }
        else if (event.type == signal_msg::PeerLeft) {
            Log("Viewer left session");
            if (consent_pending_.load()) {
                Log("Viewer left while consent pending - will reset after consent thread finishes");
            }
            ResetPeerSessionState("viewer_left");
        }
        else if (event.type == signal_msg::Offer) {
            if (consent_pending_.load()) {
                Log("Received SDP offer while consent is pending - ignoring");
                return;
            }
            if (peer_session_active_.load() && !peer_connected_) {
                Log("Received SDP offer while stale peer session was active - rebuilding peer");
                ResetPeerSessionState("offer_on_stale_peer");
            }
            session_access_mode_ = NormalizeAccessMode(ExtractJsonString(event.payload, "access_mode"));
            session_id_from_viewer_ = ExtractJsonString(event.payload, "session_id");
            Log("Received SDP offer from viewer (access_mode=%s, session_id=%s)",
                session_access_mode_.c_str(), session_id_from_viewer_.c_str());
            Log("Session negotiation path: %s sessions require %s before relay start",
                IsWatchOnlySession() ? "watch-only" : "interactive",
                IsWatchOnlySession() ? "no consent" : "user consent");
            {
                std::string session_error;
                remotex::core::SessionInfo session_info;
                if (!FetchViewerSession(&session_info, &session_error)) {
                    SendViewerError(session_error.empty()
                        ? "Falha ao validar credenciais do relay."
                        : session_error);
                    ResetPeerSessionState("viewer_session_lookup_failed");
                    return;
                }
            }
            if (!IsWatchOnlySession()) {
                // Run consent off the signaling read thread so messages keep flowing.
                pending_consent_offer_payload_ = event.payload;
                consent_pending_.store(true);
                JoinOwnedThread(consent_thread_, "consent worker replace");
                consent_thread_ = std::thread([this]() {
                    if (!EnsureInteractiveSessionApproved()) {
                        consent_pending_.store(false);
                        ResetPeerSessionState("interactive_consent_not_approved");
                        return;
                    }
                    consent_pending_.store(false);
                    if (!StartWebRTC()) {
                        SendViewerError("Falha ao iniciar o transporte relay.");
                        ResetPeerSessionState("relay_start_failed_after_consent");
                        return;
                    }
                    ConfigureDataChannelsForCurrentMode();
                    SessionDescription desc;
                    desc.type = "offer";
                    desc.sdp  = ExtractSDP(pending_consent_offer_payload_);
                    peer_.SetRemoteDescription(desc);
                    peer_.CreateAnswer();
                });
                return;
            }
            if (!StartWebRTC()) {
                SendViewerError("Falha ao iniciar o transporte relay.");
                ResetPeerSessionState("relay_start_failed");
                return;
            }
            Log("Watch-only offer accepted: starting relay without interactive consent");
            ConfigureDataChannelsForCurrentMode();
            SessionDescription desc;
            desc.type = "offer";
            desc.sdp  = ExtractSDP(event.payload);
            peer_.SetRemoteDescription(desc);
            peer_.CreateAnswer();
        }
        else if (event.type == signal_msg::Answer) {
            Log("Received SDP answer");
            SessionDescription desc;
            desc.type = "answer";
            desc.sdp  = ExtractSDP(event.payload);
            peer_.SetRemoteDescription(desc);
        }
        else if (event.type == signal_msg::ICECandidate) {
            IceCandidate candidate;
            ParseICECandidate(event.payload, candidate);
            peer_.AddIceCandidate(candidate);
        }
        else if (event.type == signal_msg::Chat) {
            // Chat via signaling (sem sessÃ£o remota ativa).
            auto extract = [&](const std::string& key) -> std::string {
                std::string needle = "\"" + key + "\":\"";
                auto pos = event.payload.find(needle);
                if (pos == std::string::npos) return {};
                pos += needle.size();
                std::string value;
                for (size_t i = pos; i < event.payload.size(); ++i) {
                    if (event.payload[i] == '\\' && i + 1 < event.payload.size()) {
                        char next = event.payload[i + 1];
                        if (next == '"') { value += '"'; ++i; }
                        else if (next == '\\') { value += '\\'; ++i; }
                        else if (next == 'n') { value += '\n'; ++i; }
                        else if (next == 'r') { value += '\r'; ++i; }
                        else { value += next; ++i; }
                    } else if (event.payload[i] == '"') {
                        break;
                    } else {
                        value += event.payload[i];
                    }
                }
                return value;
            };
            std::string sender = extract("from");
            std::string text = extract("text");
            if (!text.empty() && sender != "host") {
                chat_window_.ShowAndAddMessage(sender, text);
            }
        }
        else if (event.type == signal_msg::Error) {
            Log("Signaling server error: %s", event.error.c_str());
        }
    }

    // â”€â”€ WebRTC setup â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool CacheRelayBundle(const remotex::core::SessionInfo& session,
                          std::string* viewer_error = nullptr) {
        relay_protocol_version_ = session.relay_protocol_version;
        relay_session_id_ = session.relay_session_id;
        relay_role_ = session.relay_role;
        relay_ticket_ = session.relay_ticket;
        relay_ticket_expires_unix_ = session.relay_ticket_expires_unix;
        relay_host_override_ = session.relay_host;
        relay_port_override_ = session.relay_port;

        if (relay_session_id_.empty() || relay_ticket_.empty()) {
            if (viewer_error) {
                *viewer_error = "Sessao sem credenciais do relay.";
            }
            return false;
        }
        return true;
    }

    bool FetchViewerSession(remotex::core::SessionInfo* out_session = nullptr,
                            std::string* viewer_error = nullptr) {
        if (session_id_from_viewer_.empty()) {
            if (viewer_error) {
                *viewer_error = "Falha ao validar autorizacao do cliente.";
            }
            return false;
        }

        auto session_result = machine_client_.GetSession(session_id_from_viewer_);
        if (!session_result.session) {
            if (viewer_error) {
                *viewer_error = session_result.error.empty()
                    ? "Falha ao validar autorizacao do cliente."
                    : session_result.error;
            }
            return false;
        }

        if (!CacheRelayBundle(*session_result.session, viewer_error)) {
            return false;
        }
        if (out_session) {
            *out_session = *session_result.session;
        }
        return true;
    }

    bool StartWebRTC() {
        if (peer_session_active_.load()) {
            return true;
        }

        file_transfer_.Shutdown();
        peer_.Shutdown();
        peer_.SetListener(this);

        remotex::core::PeerConfig cfg;
        cfg.ice_servers = ice_servers_;
        cfg.session_id  = relay_session_id_.empty() ? session_id_from_viewer_ : relay_session_id_;
        cfg.is_offerer  = false;
        cfg.relay_host  = relay_host_override_.empty()
            ? g_connection_settings.relay_host
            : relay_host_override_;
        cfg.relay_port  = relay_port_override_ == 0
            ? g_connection_settings.relay_port
            : relay_port_override_;
        cfg.relay_protocol_version = relay_protocol_version_ > 0
            ? static_cast<uint32_t>(relay_protocol_version_)
            : 1;
        cfg.relay_ticket_expires_unix = relay_ticket_expires_unix_ > 0
            ? static_cast<uint64_t>(relay_ticket_expires_unix_)
            : 0;
        cfg.relay_ticket = relay_ticket_;

        if (cfg.session_id.empty() || cfg.relay_ticket.empty()) {
            Log("ERROR: Missing relay credentials for %s session",
                IsWatchOnlySession() ? "watch-only" : "interactive");
            return false;
        }

        if (!peer_.Initialize(cfg)) {
            Log("ERROR: WebRTC peer init failed for %s session",
                IsWatchOnlySession() ? "watch-only" : "interactive");
            return false;
        }
        peer_session_active_.store(true);
        first_frame_sent_ = false;
        initial_monitor_status_sent_.store(false);
        last_successful_capture_time_ = std::chrono::steady_clock::now();
        data_channels_configured_ = false;

        uint32_t w = capture_.GetWidth();
        uint32_t h = capture_.GetHeight();
        const auto rotation = capture_.GetRotation();
        if (w > 0 && h > 0) {
            peer_.AddVideoTrack(w, h, kCaptureFPS);
        }

        Log("WebRTC peer initialized for %s session, waiting for offer answer path (logical=%ux%u rotation=%d)",
            IsWatchOnlySession() ? "watch-only" : "interactive",
            w,
            h,
            static_cast<int>(rotation));
        return true;
    }

    void ConfigureDataChannelsForCurrentMode() {
        if (data_channels_configured_) {
            return;
        }
        data_channels_configured_ = true;

        peer_.CreateDataChannel("latency", true, true);
        peer_.CreateDataChannel("chat", true, true);
        peer_.CreateDataChannel("control", true, true);

        if (IsWatchOnlySession()) {
            Log("Watch-only session: interactive data channels disabled; enabled channels=[latency,chat,control]");
            file_transfer_.Shutdown();
            return;
        }

        peer_.CreateDataChannel("input", true, true);
        peer_.CreateDataChannel(remotex::core::kFilesChannel, true, true);
        peer_.CreateDataChannel("clipboard", true, true);
        Log("Interactive session: enabled channels=[latency,chat,control,input,%s,clipboard]",
            remotex::core::kFilesChannel);
    }

    // â”€â”€ WebRTCPeerListener â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void OnLocalDescription(const remotex::core::SessionDescription& desc) override {
        Log("Sending local SDP %s", desc.type.c_str());
        if (desc.type == "offer") {
            signaling_.SendOffer(machine_id_, desc.sdp);
        } else {
            signaling_.SendAnswer(machine_id_, desc.sdp);
        }
    }

    void OnIceCandidate(const remotex::core::IceCandidate& candidate) override {
        signaling_.SendICECandidate(machine_id_,
                                    candidate.candidate,
                                    candidate.sdp_mid,
                                    candidate.sdp_mline_index);
    }

    void OnConnectionStateChanged(remotex::core::PeerConnectionState state) override {
        using S = remotex::core::PeerConnectionState;
        const char* name = "unknown";
        switch (state) {
            case S::New:          name = "new"; break;
            case S::Connecting:   name = "connecting"; break;
            case S::Connected:    name = "connected"; break;
            case S::Disconnected: name = "disconnected"; break;
            case S::Failed:       name = "failed"; break;
            case S::Closed:       name = "closed"; break;
        }
        Log("WebRTC connection: %s (%s session)",
            name,
            IsWatchOnlySession() ? "watch-only" : "interactive");
        peer_connected_ = (state == S::Connected);
        if (state == S::Connected) {
            initial_monitor_status_sent_.store(false);
            // Fresh DXGI duplication for this session â€” avoids stale capture
            // that can report zero dirty rects despite screen changes.
            capture_reinit_pending_.store(true);
            StartCaptureCadenceWorker();
            last_frame_sent_time_ = std::chrono::steady_clock::now();
            last_successful_capture_time_ = std::chrono::steady_clock::now();
        } else {
            initial_monitor_status_sent_.store(false);
            StopCaptureCadenceWorker();
        }
        if (state == S::Connected || state == S::Connecting || state == S::New) {
            peer_session_active_.store(true);
        } else if (state == S::Disconnected || state == S::Failed || state == S::Closed) {
            peer_session_active_.store(false);
            file_transfer_.Shutdown();
        }

        // Inicia gravaÃ§Ã£o quando conectado.
        if (state == S::Connected && !recorder_.IsRecording()) {
            try {
                const auto rec_cfg = BuildRecorderConfig();
                if (rec_cfg.width > 0 && rec_cfg.height > 0 && !rec_cfg.output_dir.empty()) {
                    Log("Starting session recording in %s using canvas %dx%d (capture %ux%u)",
                        remotex::core::RedactPath(rec_cfg.output_dir, "recording_dir").c_str(),
                        rec_cfg.width,
                        rec_cfg.height,
                        capture_.GetWidth(),
                        capture_.GetHeight());
                    if (recorder_.Start(rec_cfg)) {
                        recording_canvas_ = {rec_cfg.width, rec_cfg.height};
                        recording_start_time_ = std::chrono::steady_clock::now();
                        Log("Session recording started");
                    } else {
#ifdef REMOTEX_HAS_FFMPEG
                        Log("Session recording failed to start. See [SessionRecorder] diagnostics above.");
#else
                        Log("Session recording unavailable in this host build: REMOTEX_HAS_FFMPEG is not defined.");
#endif
                    }
                }
            } catch (const std::exception& e) {
                Log("ERROR: Session recording startup threw: %s", e.what());
            } catch (...) {
                Log("ERROR: Session recording startup threw an unknown exception");
            }
        }
    }

    void OnIceGatheringStateChanged(remotex::core::IceGatheringState) override {}

    void OnVideoFrame(const remotex::core::VideoFrame&) override {}

    void OnDataChannelOpen(const std::string& label) override {
        Log("Data channel opened: %s", label.c_str());
        if (label == "control") {
            // Don't call SendMonitorList() here â€” we're inside a peer callback
            // that holds WebRTCPeer::mutex_, and SendDataChannelMessage would
            // try to re-acquire it, causing a deadlock in the stub transport.
            send_monitor_list_pending_.store(true);
        }
        if (!IsWatchOnlySession() && label == remotex::core::kFilesChannel) {
            try {
                std::error_code temp_dir_error;
                const auto temp_dir = fs::temp_directory_path(temp_dir_error);
                if (temp_dir_error) {
                    Log("ERROR: File transfer temp root unavailable: %s",
                        temp_dir_error.message().c_str());
                    return;
                }

                const auto recv_dir = remotex::host::EnsureDirectoryExists(
                    temp_dir / "remotex_recv",
                    "file transfer receive directory");
                if (!recv_dir.ok()) {
                    Log("ERROR: %s", recv_dir.error.c_str());
                    return;
                }

                file_transfer_.Initialize(&peer_, recv_dir.path);
                file_transfer_.SetListener(this);
                Log("File transfer ready (recv dir: %s)",
                    remotex::core::RedactPath(recv_dir.path, "recv_dir").c_str());
            } catch (const std::exception& e) {
                Log("ERROR: File transfer setup threw: %s", e.what());
            } catch (...) {
                Log("ERROR: File transfer setup threw an unknown exception");
            }
        }
    }

    void OnDataChannelClosed(const std::string& label) override {
        Log("Data channel closed: %s", label.c_str());
    }

    void OnDataChannelMessage(const remotex::core::DataChannelMessage& msg) override {
        if (msg.channel_label == "latency" && !msg.is_binary) {
            std::string text(reinterpret_cast<const char*>(msg.data.data()), msg.data.size());
            if (text.rfind("ping:", 0) == 0) {
                std::string pong = "pong:" + text.substr(5);
                peer_.SendDataChannelMessage("latency", pong);
            }
            return;
        }
        if (msg.channel_label == "chat" && !msg.is_binary) {
            std::string json(reinterpret_cast<const char*>(msg.data.data()), msg.data.size());
            // Parse "from" and "text" from JSON.
            auto extract = [&](const std::string& key) -> std::string {
                std::string needle = "\"" + key + "\":\"";
                auto pos = json.find(needle);
                if (pos == std::string::npos) return {};
                pos += needle.size();
                std::string value;
                for (size_t i = pos; i < json.size(); ++i) {
                    if (json[i] == '\\' && i + 1 < json.size()) {
                        char next = json[i + 1];
                        if (next == '"') { value += '"'; ++i; }
                        else if (next == '\\') { value += '\\'; ++i; }
                        else if (next == 'n') { value += '\n'; ++i; }
                        else if (next == 'r') { value += '\r'; ++i; }
                        else { value += next; ++i; }
                    } else if (json[i] == '"') {
                        break;
                    } else {
                        value += json[i];
                    }
                }
                return value;
            };
            std::string sender = extract("from");
            std::string text = extract("text");
            if (!text.empty()) {
                chat_window_.ShowAndAddMessage(sender, text);
            }
            return;
        }
        if (msg.channel_label == "control" && !msg.is_binary) {
            std::string json(reinterpret_cast<const char*>(msg.data.data()), msg.data.size());
            try {
                HandleControlMessage(json);
            } catch (const std::exception& e) {
                Log("ERROR: control message handling threw: %s payload=%s",
                    e.what(),
                    json.c_str());
            } catch (...) {
                Log("ERROR: control message handling threw an unknown exception payload=%s",
                    json.c_str());
            }
            return;
        }
        if (IsWatchOnlySession()) {
            return;
        }
        if (msg.channel_label == "input") {
            const auto input_result = injector_.ProcessInputData(
                std::span<const uint8_t>(msg.data.data(), msg.data.size()));
            const bool secure_desktop_input =
                !input_result.input_desktop_name.empty() &&
                _stricmp(input_result.input_desktop_name.c_str(), "default") != 0;
            const bool should_log = secure_desktop_input ||
                input_result.secure_desktop_bridge_attempted ||
                input_result.desktop_switch_attempted ||
                input_result.send_input_failures != 0 ||
                input_result.malformed_input;
            const auto now = std::chrono::steady_clock::now();
            if (should_log &&
                now - last_secure_input_diagnostic_log_ >= std::chrono::milliseconds(500)) {
                last_secure_input_diagnostic_log_ = now;
                Log("[secure_input] bytes=%zu events=%zu bridge_attempted=%s bridge_used=%s bridge_err=%lu switch_attempted=%s switch_succeeded=%s switch_err=%lu sendinput_failures=%u last_sendinput_err=%lu malformed=%s input='%s' thread='%s'",
                    msg.data.size(),
                    input_result.events_processed,
                    input_result.secure_desktop_bridge_attempted ? "yes" : "no",
                    input_result.secure_desktop_bridge_used ? "yes" : "no",
                    static_cast<unsigned long>(input_result.secure_desktop_bridge_error),
                    input_result.desktop_switch_attempted ? "yes" : "no",
                    input_result.desktop_switch_succeeded ? "yes" : "no",
                    static_cast<unsigned long>(input_result.desktop_switch_error),
                    input_result.send_input_failures,
                    static_cast<unsigned long>(input_result.last_send_input_error),
                    input_result.malformed_input ? "yes" : "no",
                    input_result.input_desktop_name.empty() ? "<unknown>" : input_result.input_desktop_name.c_str(),
                    input_result.thread_desktop_name.empty() ? "<unknown>" : input_result.thread_desktop_name.c_str());
            }
        }
        else if (msg.channel_label == remotex::core::kFilesChannel) {
            file_transfer_.OnDataReceived(msg.data, msg.is_binary);
        }
        else if (msg.channel_label == "clipboard") {
            if (!msg.is_binary) {
                CancelClipboardFileOffers("remote text clipboard superseded pending file clipboard transfer");
                std::string text(reinterpret_cast<const char*>(msg.data.data()), msg.data.size());
                clipboard_sync_.SetRemoteClipboard(text);
            }
        }
    }

    // â”€â”€ FileTransferListener â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void OnRemoteListing(const remotex::core::RemoteFsListing& listing) override {
        Log("Remote listing request served: path='%s' entries=%zu success=%d",
            listing.path.c_str(), listing.entries.size(), listing.success ? 1 : 0);
    }

    void OnTransferJobStarted(const remotex::core::TransferJobInfo& info) override {
        Log("Transfer job started job=%s destination=%s entries=%u bytes=%llu direction=%s",
            remotex::core::RedactIdentifier(info.id_hex, "job").c_str(),
            remotex::core::RedactPath(info.destination_root, "destination").c_str(),
            info.total_entries,
            static_cast<unsigned long long>(info.total_bytes),
            info.direction == remotex::core::TransferDirection::Receive ? "receive" : "send");
    }

    void OnTransferJobProgress(const remotex::core::TransferJobInfo& job,
                               const remotex::core::TransferEntryInfo& entry) override {
        double pct = job.total_bytes > 0
            ? (static_cast<double>(job.bytes_done) / static_cast<double>(job.total_bytes)) * 100.0
            : 0.0;
        Log("Transfer job job=%s pct=%.1f item=%s entries=%u/%u",
            remotex::core::RedactIdentifier(job.id_hex, "job").c_str(),
            pct,
            remotex::core::RedactPath(entry.relative_path, "item").c_str(),
            job.entries_done,
            job.total_entries);
    }

    void OnTransferJobCompleted(const remotex::core::TransferJobInfo& info,
                                const fs::path& path) override {
        Log("Transfer job completed job=%s result=%s",
            remotex::core::RedactIdentifier(info.id_hex, "job").c_str(),
            remotex::core::RedactPath(path, "result").c_str());
    }

    void OnTransferJobFailed(const remotex::core::TransferJobInfo& info,
                             const std::string& error) override {
        Log("Transfer job failed job=%s error=%s",
            remotex::core::RedactIdentifier(info.id_hex, "job").c_str(),
            error.c_str());
    }

    void OnTransferJobCancelled(const remotex::core::TransferJobInfo& info) override {
        Log("Transfer job cancelled job=%s",
            remotex::core::RedactIdentifier(info.id_hex, "job").c_str());
    }

    // â”€â”€ Monitor selection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
