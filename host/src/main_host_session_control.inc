    void SendMonitorList() {
        available_monitors_ = remotex::host::ScreenCapture::EnumerateMonitors();
        Log("Enumerated %zu monitor(s)", available_monitors_.size());

        std::string json = "{\"type\":\"monitor_list\",\"monitors\":[";
        for (size_t i = 0; i < available_monitors_.size(); ++i) {
            const auto& m = available_monitors_[i];
            if (i > 0) json += ",";
            json += "{\"adapter_index\":" + std::to_string(m.adapter_index)
                + ",\"output_index\":" + std::to_string(m.output_index)
                + ",\"name\":\"" + m.device_name + "\""
                + ",\"left\":" + std::to_string(m.left)
                + ",\"top\":" + std::to_string(m.top)
                + ",\"right\":" + std::to_string(m.right)
                + ",\"bottom\":" + std::to_string(m.bottom)
                + ",\"width\":" + std::to_string(m.width)
                + ",\"height\":" + std::to_string(m.height)
                + ",\"primary\":" + (m.primary ? "true" : "false")
                + "}";
        }
        json += "]}";
        peer_.SendDataChannelMessage("control", json);
    }

    bool SendInitialMonitorStatus() {
        // Send the Host's logical post-rotation dimensions and current DXGI
        // rotation so the Viewer stays aligned with the rotated output path
        // from the first frame onward.
        uint32_t w = capture_.GetWidth();
        uint32_t h = capture_.GetHeight();
        const DXGI_MODE_ROTATION rot = capture_.GetRotation();
        if (w == 0 || h == 0) {
            Log("WARNING: Skipping initial monitor status send because logical dimensions are empty (%ux%u rotation=%d)",
                w,
                h,
                static_cast<int>(rot));
            return false;
        }

        const int ai = static_cast<int>(capture_.GetAdapterIndex());
        const int oi = static_cast<int>(capture_.GetOutputIndex());

        std::string json = "{\"type\":\"monitor_changed\""
            ",\"adapter_index\":" + std::to_string(ai)
            + ",\"output_index\":" + std::to_string(oi)
            + ",\"width\":" + std::to_string(w)
            + ",\"height\":" + std::to_string(h)
            + ",\"rotation\":" + std::to_string(static_cast<int>(rot))
            + "}";
        const bool sent = peer_.SendDataChannelMessage("control", json);
        if (!sent) {
            Log("WARNING: Failed to send initial monitor status: %ux%u rotation=%d",
                w,
                h,
                static_cast<int>(rot));
            return false;
        }
        Log("Sent initial monitor status: %ux%u rotation=%d", w, h, static_cast<int>(rot));
        return true;
    }

    void RearmVideoBootstrapAfterDisplayContract(const char* reason) {
        ResetCaptureFrameState();
        ScheduleDeferredVideoEncoderCleanup(reason);
        Log("Rearmed host video bootstrap after display contract: %s", reason);
    }

    static bool ApplyBlockInputState(bool enable, const char* reason) {
        const BOOL ok = BlockInput(enable ? TRUE : FALSE);
        if (!ok) {
            Log("ERROR: BlockInput(%s) failed for %s: %lu",
                enable ? "TRUE" : "FALSE",
                reason,
                GetLastError());
            return false;
        }

        Log("BlockInput(%s) applied for %s",
            enable ? "TRUE" : "FALSE",
            reason);
        return true;
    }

    SecureAttentionAttemptResult AttemptSecureAttention() {
        SecureAttentionAttemptResult result;

        if (secure_attention_in_flight_.exchange(true)) {
            result.error = "request_in_progress";
            Log("Secure attention request rejected: another attempt is already in flight");
            return result;
        }

        const auto clear_in_flight = [this]() {
            secure_attention_in_flight_.store(false);
        };

        DWORD current_session_id = 0xFFFFFFFF;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session_id)) {
            result.windows_error = GetLastError();
            result.error = "session_query_failed";
            Log("Secure attention request failed: ProcessIdToSessionId err=%lu",
                result.windows_error);
            clear_in_flight();
            return result;
        }

        if (current_session_id == 0) {
            result = AttemptSecureAttentionInService();
            clear_in_flight();
            return result;
        }

        Log("Secure attention request arriving from desktop worker session %lu - forwarding to service bridge",
            static_cast<unsigned long>(current_session_id));
        result = RequestSecureAttentionFromService();
        clear_in_flight();
        return result;
    }

    std::vector<std::string> TopLevelClipboardEntryNames(const std::vector<fs::path>& paths) const {
        std::vector<std::string> entry_names;
        for (const auto& path : paths) {
            if (path.empty()) {
                continue;
            }

            const fs::path entry = path.filename().empty() ? path.root_name() : path.filename();
            const std::string entry_name = entry.string();
            if (!entry_name.empty() &&
                std::find(entry_names.begin(), entry_names.end(), entry_name) == entry_names.end()) {
                entry_names.push_back(entry_name);
            }
        }
        return entry_names;
    }

    void CancelClipboardFileOffers(const std::string& reason) {
        std::vector<std::string> tokens;
        {
            std::lock_guard<std::mutex> lock(clipboard_file_state_mutex_);
            tokens.reserve(pending_outgoing_clipboard_offers_.size() + pending_incoming_clipboard_offers_.size());
            for (const auto& [token, _] : pending_outgoing_clipboard_offers_) {
                tokens.push_back(token);
            }
            for (const auto& [token, _] : pending_incoming_clipboard_offers_) {
                if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
                    tokens.push_back(token);
                }
            }
            pending_outgoing_clipboard_offers_.clear();
            pending_incoming_clipboard_offers_.clear();
        }

        if (!peer_connected_) {
            return;
        }

        for (const auto& token : tokens) {
            json payload;
            payload["type"] = "clipboard_files_cancel";
            payload["token"] = token;
            if (!reason.empty()) {
                payload["error"] = reason;
            }
            peer_.SendDataChannelMessage("control", payload.dump());
        }
    }

    std::string GenerateClipboardTransferToken() {
        return "host-" + session_id_from_viewer_ + "-" + std::to_string(++clipboard_transfer_counter_);
    }

    void HandleClipboardFilesOffer(const std::string& raw_json) {
        std::string token;
        std::vector<std::string> entry_names;
        try {
            const json payload = json::parse(raw_json);
            token = payload.value("token", std::string{});
            if (payload.contains("entry_names") && payload["entry_names"].is_array()) {
                for (const auto& entry : payload["entry_names"]) {
                    if (!entry.is_string()) {
                        continue;
                    }
                    const std::string entry_name = entry.get<std::string>();
                    if (!entry_name.empty() &&
                        std::find(entry_names.begin(), entry_names.end(), entry_name) == entry_names.end()) {
                        entry_names.push_back(entry_name);
                    }
                }
            }
        } catch (const std::exception& e) {
            Log("ERROR: clipboard_files_offer parse failed: %s", e.what());
            return;
        }

        if (token.empty() || entry_names.empty()) {
            return;
        }

        CancelClipboardFileOffers("new viewer file clipboard offer superseded the previous session clipboard offer");

        std::lock_guard<std::mutex> lock(clipboard_file_state_mutex_);
        pending_incoming_clipboard_offers_[token] = ClipboardFileOffer{
            .token = token,
            .source_paths = {},
            .entry_names = entry_names,
        };
        Log("Clipboard files offered by viewer token=%s entries=%zu",
            token.c_str(),
            entry_names.size());
    }

    void HandleClipboardFilesPasteRequest(const std::string& raw_json) {
        std::string token;
        try {
            const json payload = json::parse(raw_json);
            token = payload.value("token", std::string{});
        } catch (const std::exception& e) {
            Log("ERROR: clipboard_files_paste_request parse failed: %s", e.what());
            return;
        }

        if (token.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(clipboard_file_state_mutex_);
            if (pending_incoming_clipboard_offers_.find(token) == pending_incoming_clipboard_offers_.end()) {
                return;
            }
            pending_incoming_clipboard_offers_.erase(token);
        }

        const auto target = remotex::core::ResolveExplorerDirectoryOrDesktop();
        if (target.path.empty()) {
            json response;
            response["type"] = "clipboard_files_cancel";
            response["token"] = token;
            response["error"] = "host destination unavailable";
            peer_.SendDataChannelMessage("control", response.dump());
            return;
        }

        json response;
        response["type"] = "clipboard_files_accept";
        response["token"] = token;
        response["destination_root"] = target.path.string();
        response["resolved_from_explorer"] = target.resolved_from_explorer;
        response["used_desktop_fallback"] = target.used_desktop_fallback;
        peer_.SendDataChannelMessage("control", response.dump());
        Log("Clipboard paste request accepted token=%s destination=%s fallback_desktop=%s",
            token.c_str(),
            target.path.string().c_str(),
            target.used_desktop_fallback ? "true" : "false");
    }

    void HandleClipboardFilesAccept(const std::string& raw_json) {
        std::string token;
        std::string destination_root;
        bool used_desktop_fallback = false;
        try {
            const json payload = json::parse(raw_json);
            token = payload.value("token", std::string{});
            destination_root = payload.value("destination_root", std::string{});
            used_desktop_fallback = payload.value("used_desktop_fallback", false);
        } catch (const std::exception& e) {
            Log("ERROR: clipboard_files_accept parse failed: %s", e.what());
            return;
        }

        ClipboardFileOffer offer;
        {
            std::lock_guard<std::mutex> lock(clipboard_file_state_mutex_);
            auto it = pending_outgoing_clipboard_offers_.find(token);
            if (it == pending_outgoing_clipboard_offers_.end()) {
                return;
            }
            offer = it->second;
            pending_outgoing_clipboard_offers_.erase(it);
        }

        if (destination_root.empty() || offer.source_paths.empty()) {
            json response;
            response["type"] = "clipboard_files_cancel";
            response["token"] = token;
            response["error"] = "host clipboard destination unavailable";
            peer_.SendDataChannelMessage("control", response.dump());
            return;
        }

        const std::string job_id = file_transfer_.SendLocalPaths(
            offer.source_paths,
            fs::path(destination_root),
            remotex::core::TransferOverwritePolicy::Allow);
        if (job_id.empty()) {
            json response;
            response["type"] = "clipboard_files_cancel";
            response["token"] = token;
            response["error"] = "host clipboard send failed";
            peer_.SendDataChannelMessage("control", response.dump());
            return;
        }

        Log("Clipboard file transfer started token=%s job=%s destination=%s fallback_desktop=%s",
            token.c_str(),
            job_id.c_str(),
            destination_root.c_str(),
            used_desktop_fallback ? "true" : "false");
    }

    void HandleClipboardFilesCancel(const std::string& raw_json) {
        std::string token;
        try {
            const json payload = json::parse(raw_json);
            token = payload.value("token", std::string{});
        } catch (const std::exception& e) {
            Log("ERROR: clipboard_files_cancel parse failed: %s", e.what());
            return;
        }

        if (token.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(clipboard_file_state_mutex_);
        pending_outgoing_clipboard_offers_.erase(token);
        pending_incoming_clipboard_offers_.erase(token);
    }

    void HandleControlMessage(const std::string& json) {
        std::string type = ExtractJsonString(json, "type");
        if (type == "select_monitor") {
            uint32_t ai = static_cast<uint32_t>(ExtractJsonInt(json, "adapter_index"));
            uint32_t oi = static_cast<uint32_t>(ExtractJsonInt(json, "output_index"));

            bool valid = false;
            for (const auto& m : available_monitors_) {
                if (m.adapter_index == ai && m.output_index == oi) {
                    valid = true;
                    break;
                }
            }
            if (!valid) {
                Log("Invalid monitor selection: adapter=%u output=%u", ai, oi);
                return;
            }

            Log("Monitor switch requested: adapter=%u output=%u", ai, oi);
            pending_adapter_index_ = ai;
            pending_output_index_ = oi;
            monitor_switch_pending_.store(true);
        }
        else if (type == "reboot_request") {
            if (IsWatchOnlySession()) {
                peer_.SendDataChannelMessage("control",
                    R"({"type":"reboot_failed","error":"watch_only_session"})");
                return;
            }

            Log("Reboot requested by viewer");
            peer_.SendDataChannelMessage("control", R"({"type":"reboot_ack"})");

            // Spawn detached thread: wait for ack delivery, then reboot.
            std::thread([]() {
                Sleep(3000);

                // Enable SE_SHUTDOWN_NAME privilege.
                HANDLE hToken = nullptr;
                if (OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
                    TOKEN_PRIVILEGES tp{};
                    LookupPrivilegeValue(nullptr, SE_SHUTDOWN_NAME,
                                         &tp.Privileges[0].Luid);
                    tp.PrivilegeCount = 1;
                    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
                    CloseHandle(hToken);
                }

                BOOL ok = InitiateSystemShutdownExW(
                    nullptr,
                    const_cast<LPWSTR>(L"RemoteX: Reinicializacao solicitada pelo suporte tecnico"),
                    0,      // timeout
                    TRUE,   // force apps to close
                    TRUE,   // reboot (not shutdown)
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER
                        | SHTDN_REASON_FLAG_PLANNED);

                if (!ok) {
                    Log("ERROR: InitiateSystemShutdownExW failed: %lu", GetLastError());
                }
            }).detach();
        }
        else if (type == "privacy_mode") {
            bool enable = (json.find("\"enabled\":true") != std::string::npos);

            if (IsWatchOnlySession()) {
                peer_.SendDataChannelMessage("control",
                    R"({"type":"privacy_mode_status","enabled":false,"error":"watch_only_session"})");
                return;
            }

            if (enable) {
                privacy_screen_.Enable();
                Log("Privacy mode enabled by viewer");
            } else {
                privacy_screen_.Disable();
                Log("Privacy mode disabled by viewer");
            }

            const bool privacy_enabled = privacy_screen_.IsEnabled();
            if (privacy_enabled != enable) {
                Log("ERROR: Privacy mode request=%s but effective state is %s",
                    enable ? "enabled" : "disabled",
                    privacy_enabled ? "enabled" : "disabled");
            }

            std::string resp = R"({"type":"privacy_mode_status","enabled":)";
            resp += (privacy_enabled ? "true" : "false");
            if (privacy_enabled != enable) {
                resp += R"(,"error":"privacy_mode_apply_failed")";
            }
            resp += "}";
            peer_.SendDataChannelMessage("control", resp);
        }
        else if (type == "secure_attention") {
            Log("Secure attention requested by viewer");
            RecordSecureTransitionStage(
                "request_received",
                "viewer requested secure attention",
                false,
                true);

            if (IsWatchOnlySession()) {
                Log("Secure attention request rejected: watch-only session");
                peer_.SendDataChannelMessage(
                    "control",
                    BuildSecureAttentionStatusPayload(false, "watch_only_session"));
                return;
            }

            const auto result = AttemptSecureAttention();
            if (result.success) {
                RecordSecureTransitionStage("sas_dispatched", "SendSAS succeeded", false, true);
                Log("Secure attention dispatch succeeded - bootstrapping capture refresh for secure-desktop transition");
                ResetCaptureFrameState();
                capture_reinit_pending_.store(true);
                RecordSecureTransitionStage(
                    "transition_bootstrap_armed",
                    "reason=secure_attention",
                    false,
                    true);
                last_successful_capture_time_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
                ScheduleDeferredVideoEncoderCleanup("secure_attention");
            } else {
                RecordSecureTransitionStage("sas_dispatch_failed",
                                            result.error.empty() ? "unknown" : result.error,
                                            true);
            }
            peer_.SendDataChannelMessage(
                "control",
                BuildSecureAttentionStatusPayload(
                    result.success,
                    result.success ? std::string{} : result.error,
                    result.windows_error != ERROR_SUCCESS
                        ? std::optional<DWORD>(result.windows_error)
                        : std::nullopt));
        }
        else if (type == "num_lock_policy") {
            const bool has_enabled = json.find("\"enabled\":") != std::string::npos;
            const bool enabled = json.find("\"enabled\":true") != std::string::npos;

            if (IsWatchOnlySession()) {
                Log("Num Lock policy request rejected: watch-only session");
                return;
            }
            if (!has_enabled) {
                Log("Num Lock policy request rejected: malformed payload=%s", json.c_str());
                return;
            }

            const auto result = injector_.AlignNumLockToPolicy(enabled);
            if (result.action == remotex::host::InputInjector::NumLockAlignmentAction::CannotDetermine ||
                !result.applied) {
                Log("Num Lock policy apply failed: requested=%s current_state=%s winerr=%lu",
                    enabled ? "enabled" : "disabled",
                    result.current_state.has_value() ? (result.current_state.value() ? "enabled" : "disabled") : "unknown",
                    result.windows_error);
                return;
            }

            Log("Num Lock policy %s: requested=%s current_state=%s",
                result.already_aligned() ? "no-op" : "applied",
                enabled ? "enabled" : "disabled",
                result.current_state.has_value() ? (result.current_state.value() ? "enabled" : "disabled") : "unknown");
        }
        else if (type == "block_input") {
            bool enable = (json.find("\"enabled\":true") != std::string::npos);
            if (IsWatchOnlySession()) {
                peer_.SendDataChannelMessage("control",
                    R"({"type":"block_input_status","enabled":false,"error":"watch_only_session"})");
                return;
            }

            const bool applied = ApplyBlockInputState(enable, "viewer_request");
            if (applied) {
                input_blocked_.store(enable);
            }

            const bool effective_enabled = input_blocked_.load();
            std::string resp = R"({"type":"block_input_status","enabled":)";
            resp += (effective_enabled ? "true" : "false");
            if (!applied) {
                resp += enable
                    ? R"(,"error":"block_input_enable_failed")"
                    : R"(,"error":"block_input_disable_failed")";
            }
            resp += "}";
            peer_.SendDataChannelMessage("control", resp);
            Log("Block input viewer request=%s effective=%s",
                enable ? "enabled" : "disabled",
                effective_enabled ? "enabled" : "disabled");
        }
        else if (type == "clipboard_files_offer") {
            HandleClipboardFilesOffer(json);
        }
        else if (type == "clipboard_files_paste_request") {
            HandleClipboardFilesPasteRequest(json);
        }
        else if (type == "clipboard_files_accept") {
            HandleClipboardFilesAccept(json);
        }
        else if (type == "clipboard_files_cancel") {
            HandleClipboardFilesCancel(json);
        }
        else if (type == "request_refresh") {
            Log("Viewer requested full refresh — resetting capture + encoder");
            ResetCaptureFrameState();
            ScheduleDeferredVideoEncoderCleanup("viewer_request_refresh");
        }
        else if (type == "viewer_status") {
            const double frame_age_ms = ExtractJsonDouble(json, "frame_age_ms");
            const double upload_fps = ExtractJsonDouble(json, "upload_fps");
            const double render_fps = ExtractJsonDouble(json, "render_fps");
            const double avg_upload_ms = ExtractJsonDouble(json, "avg_upload_ms");
            const double dropped_fps = ExtractJsonDouble(json, "dropped_fps");

            Log("[viewer_status] frame_age_ms=%.1f upload_fps=%.1f render_fps=%.1f avg_upload_ms=%.2f dropped_fps=%.2f",
                frame_age_ms, upload_fps, render_fps, avg_upload_ms, dropped_fps);

            if (peer_.GetResolutionAdaptation()) {
                const DXGI_MODE_ROTATION rotation = capture_.GetRotation();
                const bool rotated_output =
                    rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                    rotation == DXGI_MODE_ROTATION_ROTATE180 ||
                    rotation == DXGI_MODE_ROTATION_ROTATE270;
                if (rotated_output) {
                    Log("Skipping viewer-driven resolution adaptation while output rotation=%d is active",
                        static_cast<int>(rotation));
                    return;
                }
                bool downscaled = false;
                int step = 0;
                double scale_factor = 1.0;
                if (peer_.ApplyViewerResolutionFeedback(
                        avg_upload_ms,
                        dropped_fps,
                        downscaled,
                        step,
                        scale_factor)) {
                    ResetCaptureFrameState();
                    ScheduleDeferredVideoEncoderCleanup("viewer_status_resolution_feedback");
                    Log("Resolution %s to step %d (%.0f%%) after viewer feedback avg_upload_ms=%.2f dropped_fps=%.2f",
                        downscaled ? "downscaled" : "upscaled",
                        step,
                        scale_factor * 100.0,
                        avg_upload_ms,
                        dropped_fps);
                }
            }
        }
    }

    void ProcessMonitorSwitch() {
        if (!monitor_switch_pending_.exchange(false)) {
            return;
        }

        const uint32_t ai = pending_adapter_index_;
        const uint32_t oi = pending_output_index_;
        const uint32_t old_ai = capture_.GetAdapterIndex();
        const uint32_t old_oi = capture_.GetOutputIndex();

        Log("Switching capture to adapter=%u output=%u", ai, oi);
        if (!capture_.Init(ai, oi)) {
            Log("ERROR: Failed to init capture on adapter=%u output=%u, reverting", ai, oi);
            if (!capture_.Init(old_ai, old_oi)) {
                Log("FATAL: failed to restore previous monitor capture");
            }
            return;
        }

        const auto& od = capture_.GetOutputDesc();
        injector_.SetCapturedMonitorBounds(
            od.DesktopCoordinates.left,
            od.DesktopCoordinates.top,
            od.DesktopCoordinates.right,
            od.DesktopCoordinates.bottom);

        initial_monitor_status_sent_.store(false);
        if (peer_connected_) {
            StartCaptureCadenceWorker();
        }

        if (SendInitialMonitorStatus()) {
            initial_monitor_status_sent_.store(true);
            RearmVideoBootstrapAfterDisplayContract("monitor_switch_display_contract");
        } else {
            send_monitor_list_pending_.store(true);
        }

        if (peer_connected_) {
            StartCaptureCadenceWorker();
        }
    }


    // â”€â”€ JSON helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    static std::string ExtractSDP(const std::string& json) {
        auto pos = json.find("\"sdp\"");
        if (pos == std::string::npos) return json;
        pos = json.find('"', pos + 5);
        if (pos == std::string::npos) return {};
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    }

    static void ParseICECandidate(const std::string& json,
                                  remotex::core::IceCandidate& out) {
        out.candidate       = ExtractJsonString(json, "candidate");
        out.sdp_mid         = ExtractJsonString(json, "sdp_mid");
        out.sdp_mline_index = ExtractJsonInt(json, "sdp_mline_index");
    }

    static std::string ExtractJsonString(const std::string& json,
                                         const std::string& key) {
        std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos = json.find('"', pos + needle.size() + 1);
        if (pos == std::string::npos) return {};
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    }

    static int ExtractJsonInt(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return 0;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return 0;
        ++pos;
        while (pos < json.size() && json[pos] == ' ') ++pos;
        return std::atoi(json.c_str() + pos);
    }

    static double ExtractJsonDouble(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return 0.0;
        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ') ++pos;
        return std::atof(json.c_str() + pos);
    }

    struct ClipboardFileOffer {
        std::string               token;
        std::vector<fs::path>     source_paths;
        std::vector<std::string>  entry_names;
    };

    // â”€â”€ Members â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    remotex::core::AuthClient*     auth_;
    std::string                    machine_id_;
    std::string                    machine_key_;

    remotex::core::MachineClient   machine_client_;
    remotex::core::TurnClient      turn_client_;
    std::vector<remotex::core::IceServer> ice_servers_;

    remotex::host::ScreenCapture   capture_;
    remotex::host::InputInjector   injector_;
    std::unique_ptr<remotex::host::InputInjector::SecureDesktopBridge> secure_desktop_input_bridge_;
    remotex::core::SignalingClient signaling_;
    remotex::core::WebRTCPeer      peer_;
    remotex::core::FileTransfer    file_transfer_;
    remotex::host::ChatWindow      chat_window_;
    remotex::host::ClipboardSync   clipboard_sync_;
    std::map<std::string, ClipboardFileOffer> pending_outgoing_clipboard_offers_;
    std::map<std::string, ClipboardFileOffer> pending_incoming_clipboard_offers_;
    uint64_t                       clipboard_transfer_counter_ = 0;
    std::mutex                     clipboard_file_state_mutex_;

    std::atomic<bool>              peer_connected_{false};
    std::atomic<bool>              peer_session_active_{false};
    bool                           first_frame_sent_ = false;
    uint32_t                       rotated_bootstrap_full_refreshes_remaining_ = 0;
    bool                           data_channels_configured_ = false;
    std::string                    session_access_mode_ = std::string(remotex::core::kSessionAccessModeInteractive);
    std::string                    session_id_from_viewer_;
    int                            relay_protocol_version_ = 0;
    std::string                    relay_session_id_;
    std::string                    relay_role_;
    std::string                    relay_ticket_;
    int64_t                        relay_ticket_expires_unix_ = 0;
    std::string                    relay_host_override_;
    uint16_t                       relay_port_override_ = 0;
    std::chrono::steady_clock::time_point next_signaling_refresh_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_capture_retry_{};
    std::chrono::steady_clock::time_point last_secure_input_diagnostic_log_{};
    std::chrono::steady_clock::time_point last_secure_transition_log_{};
    std::string                    secure_transition_stage_;
    std::string                    secure_transition_detail_;
    bool                           secure_transition_failure_ = false;
    std::string                    secure_transition_last_success_stage_;
    std::string                    secure_transition_last_success_detail_;
    std::string                    secure_transition_last_failure_stage_;
    std::string                    secure_transition_last_failure_detail_;
    std::string                    last_logged_secure_transition_stage_;
    std::string                    last_logged_secure_transition_detail_;
    bool                           last_logged_secure_transition_failure_ = false;

    // Async consent: offer payload saved while consent dialog runs off the read thread.
    std::string                    pending_consent_offer_payload_;
    std::atomic<bool>              consent_pending_{false};
    std::thread                    consent_thread_;

    // â”€â”€ Monitor selection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    std::vector<remotex::host::MonitorInfo> available_monitors_;
    std::atomic<bool>              monitor_switch_pending_{false};
    std::atomic<bool>              send_monitor_list_pending_{false};
    std::atomic<bool>              initial_monitor_status_sent_{false};
    std::atomic<bool>              deferred_video_encoder_cleanup_pending_{false};
    uint32_t                       pending_adapter_index_ = 0;
    uint32_t                       pending_output_index_ = 0;

    // â”€â”€ Privacy mode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    remotex::host::PrivacyScreen     privacy_screen_;

    // â”€â”€ Block input â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    std::atomic<bool>                input_blocked_{false};
    std::atomic<bool>                secure_attention_in_flight_{false};

    // â”€â”€ Session recording â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    remotex::host::SessionRecorder   recorder_;
    remotex::host::RecordingUploader recording_uploader_;
    remotex::host::RecordingCanvas   recording_canvas_{};
    std::chrono::steady_clock::time_point recording_start_time_{};
    std::mutex                       capture_state_mutex_;
    std::mutex                       capture_cadence_thread_mutex_;
    remotex::host::CaptureCadenceController capture_cadence_;
    std::thread                      capture_cadence_thread_;
    std::atomic<bool>                capture_cadence_running_{false};
    std::atomic<bool>                capture_cadence_restart_requested_{false};
    std::vector<uint8_t>             capture_bgra_;
    uint32_t                         capture_bgra_width_ = 0;
    uint32_t                         capture_bgra_height_ = 0;
    uint32_t                         capture_bgra_pitch_ = 0;
    remotex::host::FrameRotator      frame_rotator_;
    bool                             frame_rotator_initialized_ = false;
    std::optional<remotex::host::CopyRegion>        last_pointer_region_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO  last_pointer_shape_info_{};
    bool                             last_pointer_visible_ = false;
    CapturePerfWindow                capture_perf_{};
    std::chrono::steady_clock::time_point last_frame_sent_time_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_successful_capture_time_{std::chrono::steady_clock::now()};
    std::atomic<bool>                capture_reinit_pending_{false};
    std::optional<remotex::core::VideoFrameMetrics> last_video_frame_metrics_;
    std::optional<PreparedCaptureFrame> latest_prepared_frame_;
    uint64_t                         last_recorded_frame_id_ = 0;
    uint64_t                         last_published_frame_id_ = 0;
    std::chrono::steady_clock::time_point last_rotated_bootstrap_gdi_attempt_{};
    std::chrono::steady_clock::time_point last_gdi_fallback_attempt_{};
    std::chrono::steady_clock::time_point last_gdi_fallback_transition_log_{};
