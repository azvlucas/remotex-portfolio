    struct CapturePerfWindow {
        std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
        uint64_t capture_attempt_count = 0;
        uint64_t capture_hit_count = 0;
        uint64_t captured_frames = 0;
        uint64_t encoded_frames = 0;
        uint64_t sent_frames = 0;
        uint64_t timeout_capture_count = 0;
        uint64_t idle_skipped = 0;
        uint64_t dropped_frames = 0;
        uint64_t copied_bytes = 0;
        uint64_t dirty_rects = 0;
        uint64_t move_rects = 0;
        uint64_t last_frame_id = 0;
        uint64_t desktop_switch_count = 0;
        uint64_t desktop_context_unavailable_count = 0;
        uint64_t fallback_snapshot_success_count = 0;
        uint64_t fallback_snapshot_failure_count = 0;
        int quality_level = 0;
        double acquire_wait_ms = 0.0;
        double accumulated_frames = 0.0;
        double capture_ms = 0.0;
        double encode_ms = 0.0;
        double map_ms = 0.0;
        double copy_ms = 0.0;
        double overlay_ms = 0.0;
        double send_ms = 0.0;
        std::string last_capture_status;
        std::string previous_input_desktop_name;
        std::string input_desktop_name;
        std::string thread_desktop_name;
        bool input_matches_thread = false;
    };

    struct PreparedCaptureFrame {
        remotex::core::VideoFrameMetrics metrics{};
        uint64_t frame_id = 0;
        bool should_record = false;
        bool uses_rotated_output = false;
        uint32_t output_width = 0;
        uint32_t output_height = 0;
        uint32_t output_pitch = 0;
        size_t output_size = 0;
        std::vector<uint8_t> output_buffer;
        std::vector<remotex::host::CopyRegion> logical_dirty_regions;
        std::vector<DXGI_OUTDUPL_MOVE_RECT> logical_move_rects;
        std::optional<remotex::host::CopyRegion> logical_pointer_region;
        std::optional<POINT> logical_pointer_position;
    };

    static int64_t SteadyClockNowMicros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static const char* CaptureStatusName(remotex::host::CaptureStatus status) {
        switch (status) {
            case remotex::host::CaptureStatus::Ok:
                return "ok";
            case remotex::host::CaptureStatus::Timeout:
                return "timeout";
            case remotex::host::CaptureStatus::DisplayChanged:
                return "display_changed";
            case remotex::host::CaptureStatus::DesktopSwitched:
                return "desktop_switched";
            case remotex::host::CaptureStatus::DesktopContextUnavailable:
                return "desktop_context_unavailable";
            case remotex::host::CaptureStatus::DeviceLost:
                return "device_lost";
            case remotex::host::CaptureStatus::AccessDenied:
                return "access_denied";
            case remotex::host::CaptureStatus::Error:
            default:
                return "error";
        }
    }

    void RecordCaptureDiagnosticStateLocked(const remotex::host::CaptureResult& capture_result) {
        capture_perf_.last_capture_status = CaptureStatusName(capture_result.status);
        capture_perf_.previous_input_desktop_name = capture_result.previous_input_desktop_name;
        capture_perf_.input_desktop_name = capture_result.input_desktop_name;
        capture_perf_.thread_desktop_name = capture_result.thread_desktop_name;
        capture_perf_.input_matches_thread = capture_result.input_matches_thread;
        if (capture_result.status == remotex::host::CaptureStatus::DesktopSwitched) {
            capture_perf_.desktop_switch_count++;
        }
        if (capture_result.status == remotex::host::CaptureStatus::DesktopContextUnavailable) {
            capture_perf_.desktop_context_unavailable_count++;
        }
    }

    void LogCaptureDesktopContext(const char* prefix,
                                 const remotex::host::CaptureResult& capture_result) {
        Log("%s previous_input='%s' input='%s' thread='%s' match=%s input_err=%lu thread_err=%lu",
            prefix,
            capture_result.previous_input_desktop_name.empty()
                ? "<unknown>"
                : capture_result.previous_input_desktop_name.c_str(),
            capture_result.input_desktop_name.empty()
                ? "<unknown>"
                : capture_result.input_desktop_name.c_str(),
            capture_result.thread_desktop_name.empty()
                ? "<unknown>"
                : capture_result.thread_desktop_name.c_str(),
            capture_result.input_matches_thread ? "yes" : "no",
            static_cast<unsigned long>(capture_result.input_desktop_error),
            static_cast<unsigned long>(capture_result.thread_desktop_error));
    }

    static std::chrono::milliseconds SecureTransitionLogIntervalForStage(const char* stage) {
        if (stage == nullptr || *stage == '\0') {
            return std::chrono::milliseconds(500);
        }
        if (_stricmp(stage, "capture_timeout") == 0 ||
            _stricmp(stage, "secure_desktop_timeout") == 0 ||
            _stricmp(stage, "send_idle_no_frame") == 0 ||
            _stricmp(stage, "prepared_frame_published") == 0) {
            return std::chrono::seconds(2);
        }
        if (_stricmp(stage, "fallback_snapshot_started") == 0 ||
            _stricmp(stage, "fallback_snapshot_failed") == 0 ||
            _stricmp(stage, "fallback_snapshot_succeeded") == 0) {
            return std::chrono::seconds(1);
        }
        return std::chrono::milliseconds(500);
    }

    static bool SecureTransitionDetailForcesImmediateLog(const char* stage) {
        if (stage == nullptr || *stage == '\0') {
            return false;
        }
        return _stricmp(stage, "capture_timeout") != 0 &&
               _stricmp(stage, "secure_desktop_timeout") != 0 &&
               _stricmp(stage, "send_idle_no_frame") != 0 &&
               _stricmp(stage, "prepared_frame_published") != 0 &&
               _stricmp(stage, "fallback_snapshot_started") != 0 &&
               _stricmp(stage, "fallback_snapshot_failed") != 0 &&
               _stricmp(stage, "fallback_snapshot_succeeded") != 0;
    }

    void RecordSecureTransitionStage(const char* stage,
                                     std::string detail = {},
                                     bool failure = false,
                                     bool checkpoint = false) {
        if (stage == nullptr || *stage == '\0') {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto min_log_interval = SecureTransitionLogIntervalForStage(stage);
        const bool detail_triggers_log =
            SecureTransitionDetailForcesImmediateLog(stage);
        bool should_log = false;
        bool failure_to_log = false;
        std::string stage_to_log;
        std::string detail_to_log;
        {
            std::lock_guard lock(capture_state_mutex_);
            secure_transition_stage_ = stage;
            secure_transition_detail_ = std::move(detail);
            secure_transition_failure_ = failure;

            if (checkpoint && !failure) {
                secure_transition_last_success_stage_ = secure_transition_stage_;
                secure_transition_last_success_detail_ = secure_transition_detail_;
            }
            if (failure) {
                secure_transition_last_failure_stage_ = secure_transition_stage_;
                secure_transition_last_failure_detail_ = secure_transition_detail_;
            }

            const bool stage_changed =
                last_logged_secure_transition_stage_ != secure_transition_stage_;
            const bool detail_changed =
                last_logged_secure_transition_detail_ != secure_transition_detail_;
            const bool failure_changed =
                last_logged_secure_transition_failure_ != secure_transition_failure_;
            const bool interval_elapsed =
                now - last_secure_transition_log_ >= min_log_interval;
            if (stage_changed ||
                failure_changed ||
                (detail_triggers_log && detail_changed) ||
                interval_elapsed) {
                last_logged_secure_transition_stage_ = secure_transition_stage_;
                last_logged_secure_transition_detail_ = secure_transition_detail_;
                last_logged_secure_transition_failure_ = secure_transition_failure_;
                last_secure_transition_log_ = now;
                stage_to_log = secure_transition_stage_;
                detail_to_log = secure_transition_detail_;
                failure_to_log = secure_transition_failure_;
                should_log = true;
            }
        }

        if (should_log) {
            Log("[secure_transition] stage=%s failure=%s detail=%s",
                stage_to_log.c_str(),
                failure_to_log ? "yes" : "no",
                detail_to_log.empty() ? "<none>" : detail_to_log.c_str());
        }
    }

    static DXGI_MODE_ROTATION InverseRotation(DXGI_MODE_ROTATION rotation) {
        switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            return DXGI_MODE_ROTATION_ROTATE270;
        case DXGI_MODE_ROTATION_ROTATE180:
            return DXGI_MODE_ROTATION_ROTATE180;
        case DXGI_MODE_ROTATION_ROTATE270:
            return DXGI_MODE_ROTATION_ROTATE90;
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        default:
            return DXGI_MODE_ROTATION_IDENTITY;
        }
    }

    bool TryPrepareRotatedBootstrapFrameFromGdi(const remotex::host::CapturedFrame& frame,
                                                const std::chrono::steady_clock::time_point capture_done) {
        const DXGI_MODE_ROTATION rotation = capture_.GetRotation();
        const bool rotated_output =
            rotation == DXGI_MODE_ROTATION_ROTATE90 ||
            rotation == DXGI_MODE_ROTATION_ROTATE180 ||
            rotation == DXGI_MODE_ROTATION_ROTATE270;
        if (!rotated_output ||
            first_frame_sent_ ||
            !initial_monitor_status_sent_.load()) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_rotated_bootstrap_gdi_attempt_ < std::chrono::milliseconds(250)) {
            return false;
        }
        last_rotated_bootstrap_gdi_attempt_ = now;

        const auto& output_desc = capture_.GetOutputDesc();
        const RECT region = output_desc.DesktopCoordinates;
        remotex::host::GdiDesktopSnapshot snapshot;
        DWORD gdi_error = ERROR_SUCCESS;
        if (!remotex::host::CaptureDesktopRegionGdi(region, snapshot, gdi_error)) {
            Log("WARNING: Rotated bootstrap GDI snapshot failed: err=%lu region=[%ld,%ld,%ld,%ld] rotation=%d",
                static_cast<unsigned long>(gdi_error),
                region.left,
                region.top,
                region.right,
                region.bottom,
                static_cast<int>(rotation));
            return false;
        }
        if (snapshot.width == 0 || snapshot.height == 0 || snapshot.bgra.empty()) {
            Log("WARNING: Rotated bootstrap GDI snapshot was empty: size=%ux%u pitch=%u rotation=%d",
                snapshot.width,
                snapshot.height,
                snapshot.pitch,
                static_cast<int>(rotation));
            return false;
        }

        if (!frame_rotator_initialized_) {
            frame_rotator_initialized_ = frame_rotator_.Initialize(capture_.GetDevice());
        }
        if (!frame_rotator_initialized_ ||
            !frame_rotator_.Rotate(
                snapshot.bgra.data(),
                snapshot.width,
                snapshot.height,
                snapshot.pitch,
                InverseRotation(rotation))) {
            Log("WARNING: Rotated bootstrap raw-cache seed failed: snapshot=%ux%u pitch=%u rotation=%d",
                snapshot.width,
                snapshot.height,
                snapshot.pitch,
                static_cast<int>(rotation));
            return false;
        }

        const uint32_t raw_width = frame_rotator_.GetRotatedWidth();
        const uint32_t raw_height = frame_rotator_.GetRotatedHeight();
        const uint32_t raw_pitch = frame_rotator_.GetRotatedPitch();
        const size_t raw_size = static_cast<size_t>(raw_pitch) * raw_height;
        if (raw_width == 0 || raw_height == 0 || raw_pitch == 0 || raw_size == 0) {
            Log("WARNING: Rotated bootstrap raw-cache seed produced invalid output: size=%ux%u pitch=%u rotation=%d",
                raw_width,
                raw_height,
                raw_pitch,
                static_cast<int>(rotation));
            return false;
        }

        const auto metrics = remotex::host::MakeVideoFrameMetrics(
            frame.frame_index,
            SteadyClockNowMicros());
        {
            std::lock_guard lock(capture_state_mutex_);
            EnsureCaptureBufferLocked(raw_width, raw_height, raw_pitch);
            if (capture_bgra_.size() != raw_size) {
                capture_bgra_.assign(raw_size, 0);
                capture_bgra_width_ = raw_width;
                capture_bgra_height_ = raw_height;
                capture_bgra_pitch_ = raw_pitch;
            }
            std::memcpy(capture_bgra_.data(), frame_rotator_.GetRotatedData(), raw_size);

            PreparedCaptureFrame prepared_frame{
                .metrics = metrics,
                .frame_id = frame.frame_index,
                .should_record = true,
                .uses_rotated_output = true,
                .output_width = snapshot.width,
                .output_height = snapshot.height,
                .output_pitch = snapshot.pitch,
                .output_size = snapshot.bgra.size(),
                .output_buffer = snapshot.bgra,
            };

            prepared_frame.logical_dirty_regions.push_back(remotex::host::CopyRegion{
                .left = 0,
                .top = 0,
                .right = static_cast<LONG>(snapshot.width),
                .bottom = static_cast<LONG>(snapshot.height),
            });

            if (frame.pointer_visible) {
                const LONG draw_height =
                    frame.pointer_shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
                        ? static_cast<LONG>(frame.pointer_shape_info.Height / 2)
                        : static_cast<LONG>(frame.pointer_shape_info.Height);
                const POINT logical_pointer = remotex::host::MapPointerDrawOriginIntoDesktopRegion(
                    POINT{frame.pointer_x, frame.pointer_y},
                    frame.pointer_shape_info.HotSpot,
                    region);
                prepared_frame.logical_pointer_position = logical_pointer;
                remotex::host::ScreenCapture::OverlayPointerShape(
                    std::span<uint8_t>(
                        prepared_frame.output_buffer.data(),
                        prepared_frame.output_buffer.size()),
                    prepared_frame.output_pitch,
                    prepared_frame.output_width,
                    prepared_frame.output_height,
                    logical_pointer,
                    frame.pointer_shape_info,
                    std::span<const uint8_t>(
                        frame.pointer_shape_buffer.data(),
                        frame.pointer_shape_buffer.size()));

                auto raw_pointer_region = remotex::host::ClipRegion(
                    remotex::host::CopyRegion{
                        .left = frame.pointer_x - frame.pointer_shape_info.HotSpot.x,
                        .top = frame.pointer_y - frame.pointer_shape_info.HotSpot.y,
                        .right = frame.pointer_x - frame.pointer_shape_info.HotSpot.x +
                            static_cast<LONG>(frame.pointer_shape_info.Width),
                        .bottom = frame.pointer_y - frame.pointer_shape_info.HotSpot.y + draw_height,
                    },
                    frame.width,
                    frame.height);
                if (remotex::host::IsValidRegion(raw_pointer_region)) {
                    last_pointer_region_ = raw_pointer_region;
                } else {
                    last_pointer_region_.reset();
                }
                last_pointer_visible_ = true;
                last_pointer_shape_info_ = frame.pointer_shape_info;
            } else {
                last_pointer_region_.reset();
                last_pointer_visible_ = false;
                last_pointer_shape_info_ = {};
            }

            last_video_frame_metrics_ = metrics;
            latest_prepared_frame_ = std::move(prepared_frame);
            capture_perf_.capture_hit_count++;
            capture_perf_.captured_frames++;
            capture_perf_.copied_bytes += raw_size;
            capture_perf_.last_frame_id = frame.frame_index;
            rotated_bootstrap_full_refreshes_remaining_ = 0;
        }

        last_successful_capture_time_ = capture_done;
        RecordSecureTransitionStage(
            "rotated_bootstrap_gdi_seeded",
            std::string("logical=") + std::to_string(snapshot.width) + "x" +
                std::to_string(snapshot.height) +
                " raw=" + std::to_string(raw_width) + "x" + std::to_string(raw_height) +
                " rotation=" + std::to_string(static_cast<int>(rotation)),
            false,
            true);
        SendCaptureFrame();
        return true;
    }

    bool TryPrepareSecureDesktopFallbackFrame(const remotex::host::CaptureResult& capture_result,
                                              const std::chrono::steady_clock::time_point capture_done) {
        if (!capture_result.input_desktop_available ||
            capture_result.input_desktop_name.empty() ||
            _stricmp(capture_result.input_desktop_name.c_str(), "default") == 0 ||
            !capture_result.input_matches_thread) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_gdi_fallback_attempt_ < std::chrono::milliseconds(250)) {
            return false;
        }
        last_gdi_fallback_attempt_ = now;

        const auto& output_desc = capture_.GetOutputDesc();
        const RECT region = output_desc.DesktopCoordinates;
        if (now - last_gdi_fallback_transition_log_ >= std::chrono::seconds(1)) {
            last_gdi_fallback_transition_log_ = now;
            Log("[secure_transition] stage=fallback_snapshot_started failure=no detail=desktop=%s region=[%ld,%ld,%ld,%ld]",
                capture_result.input_desktop_name.c_str(),
                region.left,
                region.top,
                region.right,
                region.bottom);
        }
        remotex::host::GdiDesktopSnapshot snapshot;
        DWORD gdi_error = ERROR_SUCCESS;
        if (!remotex::host::CaptureDesktopRegionGdi(region, snapshot, gdi_error)) {
            RecordSecureTransitionStage(
                "fallback_snapshot_failed",
                std::string("desktop=") + capture_result.input_desktop_name +
                    " err=" + std::to_string(gdi_error) +
                    " region=[" + std::to_string(region.left) + "," +
                    std::to_string(region.top) + "," +
                    std::to_string(region.right) + "," +
                    std::to_string(region.bottom) + "]",
                true);
            {
                std::lock_guard lock(capture_state_mutex_);
                capture_perf_.fallback_snapshot_failure_count++;
            }
            return false;
        }

        const auto metrics = remotex::host::MakeVideoFrameMetrics(
            ++last_published_frame_id_,
            SteadyClockNowMicros());
        {
            std::lock_guard lock(capture_state_mutex_);
            EnsureCaptureBufferLocked(snapshot.width, snapshot.height, snapshot.pitch);
            if (capture_bgra_.size() != snapshot.bgra.size()) {
                capture_bgra_.assign(snapshot.bgra.size(), 0);
                capture_bgra_width_ = snapshot.width;
                capture_bgra_height_ = snapshot.height;
                capture_bgra_pitch_ = snapshot.pitch;
            }
            std::memcpy(capture_bgra_.data(), snapshot.bgra.data(), snapshot.bgra.size());

            PreparedCaptureFrame prepared_frame{
                .metrics = metrics,
                .frame_id = last_published_frame_id_,
                .should_record = true,
                .uses_rotated_output = false,
                .output_width = snapshot.width,
                .output_height = snapshot.height,
                .output_pitch = snapshot.pitch,
                .output_size = snapshot.bgra.size(),
            };
            last_video_frame_metrics_ = metrics;
            latest_prepared_frame_ = std::move(prepared_frame);
            last_pointer_region_.reset();
            last_pointer_visible_ = false;
            last_pointer_shape_info_ = {};
            capture_perf_.capture_hit_count++;
            capture_perf_.captured_frames++;
            capture_perf_.fallback_snapshot_success_count++;
            capture_perf_.last_frame_id = last_published_frame_id_;
        }

        last_successful_capture_time_ = capture_done;
        RecordSecureTransitionStage(
            "fallback_snapshot_succeeded",
            std::string("desktop=") + capture_result.input_desktop_name +
                " size=" + std::to_string(snapshot.width) + "x" +
                std::to_string(snapshot.height) +
                " pitch=" + std::to_string(snapshot.pitch),
            false,
            true);
        return true;
    }

    bool EnsureCaptureBufferLocked(uint32_t width, uint32_t height, uint32_t pitch) {
        if (capture_bgra_width_ == width &&
            capture_bgra_height_ == height &&
            capture_bgra_pitch_ == pitch &&
            !capture_bgra_.empty()) {
            return false;
        }

        capture_bgra_width_ = width;
        capture_bgra_height_ = height;
        capture_bgra_pitch_ = pitch;
        capture_bgra_.assign(static_cast<size_t>(pitch) * height, 0);
        last_pointer_region_.reset();
        last_pointer_visible_ = false;
        last_pointer_shape_info_ = {};
        return true;
    }

    bool EnsureCaptureBuffer(uint32_t width, uint32_t height, uint32_t pitch) {
        std::lock_guard lock(capture_state_mutex_);
        return EnsureCaptureBufferLocked(width, height, pitch);
    }

    void ResetCaptureFrameStateLocked() {
        capture_bgra_.clear();
        capture_bgra_width_ = 0;
        capture_bgra_height_ = 0;
        capture_bgra_pitch_ = 0;
        frame_rotator_.Reset();
        frame_rotator_initialized_ = false;
        first_frame_sent_ = false;
        last_pointer_region_.reset();
        last_pointer_visible_ = false;
        last_pointer_shape_info_ = {};
        latest_prepared_frame_.reset();
        last_video_frame_metrics_.reset();
        last_recorded_frame_id_ = 0;
        const DXGI_MODE_ROTATION rotation = capture_.GetRotation();
        rotated_bootstrap_full_refreshes_remaining_ =
            (rotation == DXGI_MODE_ROTATION_ROTATE90 ||
             rotation == DXGI_MODE_ROTATION_ROTATE180 ||
             rotation == DXGI_MODE_ROTATION_ROTATE270)
            ? 2u
            : 0u;
    }

    void ResetCaptureFrameState() {
        std::lock_guard lock(capture_state_mutex_);
        ResetCaptureFrameStateLocked();
    }

    void ScheduleDeferredVideoEncoderCleanup(const char* reason) {
        const bool already_pending = deferred_video_encoder_cleanup_pending_.exchange(true);
        if (already_pending) {
            Log("Deferred video encoder cleanup already pending (latest reason=%s)", reason);
            return;
        }
        Log("Deferred video encoder cleanup scheduled: %s", reason);
    }

    void ProcessDeferredVideoEncoderCleanup() {
        if (!deferred_video_encoder_cleanup_pending_.exchange(false)) {
            return;
        }
        Log("Applying deferred video encoder cleanup");
        peer_.CleanupVideoEncoder();
    }

    void MaybeLogCapturePerf() {
        remotex::host::SessionInstrumentationSummary summary;
        {
            std::lock_guard lock(capture_state_mutex_);
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - capture_perf_.started;
            if (elapsed < std::chrono::seconds(5)) {
                return;
            }

            summary = remotex::host::SummarizeInstrumentationWindow({
                .window_seconds = std::chrono::duration<double>(elapsed).count(),
                .capture_attempt_count = capture_perf_.capture_attempt_count,
                .capture_hit_count = capture_perf_.capture_hit_count,
                .captured_frames = capture_perf_.captured_frames,
                .encoded_frames = capture_perf_.encoded_frames,
                .sent_frames = capture_perf_.sent_frames,
                .timeout_capture_count = capture_perf_.timeout_capture_count,
                .idle_skipped = capture_perf_.idle_skipped,
                .dropped_frames = capture_perf_.dropped_frames + peer_.ResetFrameDropCount(),
                .copied_bytes = capture_perf_.copied_bytes,
                .dirty_rects = capture_perf_.dirty_rects,
                .move_rects = capture_perf_.move_rects,
                .last_frame_id = capture_perf_.last_frame_id,
                .desktop_switch_count = capture_perf_.desktop_switch_count,
                .desktop_context_unavailable_count = capture_perf_.desktop_context_unavailable_count,
                .fallback_snapshot_success_count = capture_perf_.fallback_snapshot_success_count,
                .fallback_snapshot_failure_count = capture_perf_.fallback_snapshot_failure_count,
                .quality_level = peer_.GetEncodeQualityStep(),
                .acquire_wait_ms = capture_perf_.acquire_wait_ms,
                .accumulated_frames = capture_perf_.accumulated_frames,
                .capture_ms = capture_perf_.capture_ms,
                .encode_ms = capture_perf_.encode_ms,
                .map_ms = capture_perf_.map_ms,
                .copy_ms = capture_perf_.copy_ms,
                .overlay_ms = capture_perf_.overlay_ms,
                .send_ms = capture_perf_.send_ms,
                .seconds_since_last_successful_capture = std::chrono::duration<double>(
                    now - last_successful_capture_time_).count(),
                .input_matches_thread = capture_perf_.input_matches_thread,
                .secure_transition_failure = secure_transition_failure_,
                .last_capture_status = capture_perf_.last_capture_status,
                .previous_input_desktop_name = capture_perf_.previous_input_desktop_name,
                .input_desktop_name = capture_perf_.input_desktop_name,
                .thread_desktop_name = capture_perf_.thread_desktop_name,
                .secure_transition_stage = secure_transition_stage_,
                .secure_transition_detail = secure_transition_detail_,
                .last_successful_transition_stage = secure_transition_last_success_stage_,
                .last_successful_transition_detail = secure_transition_last_success_detail_,
                .last_failed_transition_stage = secure_transition_last_failure_stage_,
                .last_failed_transition_reason = secure_transition_last_failure_detail_,
            });
            capture_perf_ = {};
        }

        Log("[capture] capture_fps=%.1f capture_attempt_fps=%.1f capture_hit_ratio=%.3f encoded_fps=%.1f sent_fps=%.1f idle=%llu copy_mb_s=%.1f avg_acquire_wait_ms=%.2f timeout_capture_count=%llu avg_accumulated_frames=%.2f avg_capture_ms=%.2f avg_encode_ms=%.2f avg_map_ms=%.2f avg_copy_ms=%.2f avg_overlay_ms=%.2f avg_send_ms=%.2f avg_dirty_rects=%.2f avg_move_rects=%.2f encode_stage=bounded_slot dropped_frames=%llu quality_level=%d last_frame_id=%llu",
            summary.capture_fps,
            summary.capture_attempt_fps,
            summary.capture_hit_ratio,
            summary.encoded_fps,
            summary.sent_fps,
            static_cast<unsigned long long>(summary.idle_skipped),
            summary.copy_mb_s,
            summary.avg_acquire_wait_ms,
            static_cast<unsigned long long>(summary.timeout_capture_count),
            summary.avg_accumulated_frames,
            summary.avg_capture_ms,
            summary.avg_encode_ms,
            summary.avg_map_ms,
            summary.avg_copy_ms,
            summary.avg_overlay_ms,
            summary.avg_send_ms,
            summary.avg_dirty_rects,
            summary.avg_move_rects,
            static_cast<unsigned long long>(summary.dropped_frames),
            summary.quality_level,
            static_cast<unsigned long long>(summary.last_frame_id));
        Log("[capture_context] status=%s desktop_switch_count=%llu desktop_context_unavailable_count=%llu fallback_snapshot_success_count=%llu fallback_snapshot_failure_count=%llu previous_input='%s' input='%s' thread='%s' match=%s seconds_since_last_success=%.2f",
            summary.last_capture_status.empty() ? "<unknown>" : summary.last_capture_status.c_str(),
            static_cast<unsigned long long>(summary.desktop_switch_count),
            static_cast<unsigned long long>(summary.desktop_context_unavailable_count),
            static_cast<unsigned long long>(summary.fallback_snapshot_success_count),
            static_cast<unsigned long long>(summary.fallback_snapshot_failure_count),
            summary.previous_input_desktop_name.empty() ? "<unknown>" : summary.previous_input_desktop_name.c_str(),
            summary.input_desktop_name.empty() ? "<unknown>" : summary.input_desktop_name.c_str(),
            summary.thread_desktop_name.empty() ? "<unknown>" : summary.thread_desktop_name.c_str(),
            summary.input_matches_thread ? "yes" : "no",
            summary.seconds_since_last_successful_capture);
        Log("[secure_transition_state] current_stage=%s current_failure=%s current_detail=%s last_success_stage=%s last_success_detail=%s last_failure_stage=%s last_failure_reason=%s",
            summary.secure_transition_stage.empty() ? "<unset>" : summary.secure_transition_stage.c_str(),
            summary.secure_transition_failure ? "yes" : "no",
            summary.secure_transition_detail.empty() ? "<none>" : summary.secure_transition_detail.c_str(),
            summary.last_successful_transition_stage.empty() ? "<unset>" : summary.last_successful_transition_stage.c_str(),
            summary.last_successful_transition_detail.empty() ? "<none>" : summary.last_successful_transition_detail.c_str(),
            summary.last_failed_transition_stage.empty() ? "<unset>" : summary.last_failed_transition_stage.c_str(),
            summary.last_failed_transition_reason.empty() ? "<none>" : summary.last_failed_transition_reason.c_str());

        if (peer_session_active_.load() && peer_connected_.load()) {
            std::string diagnostics_json =
                remotex::host::BuildSessionDiagnosticsJson(summary);
            if (peer_.GetResolutionAdaptation() && !diagnostics_json.empty() &&
                diagnostics_json.back() == '}') {
                diagnostics_json.pop_back();
                diagnostics_json += ",\"resolution_scale\":"
                    + std::to_string(peer_.GetResolutionScaleFactor());
                diagnostics_json += ",\"resolution_step\":"
                    + std::to_string(peer_.GetResolutionAdaptationStep());
                diagnostics_json += "}";
            }
            peer_.SendDataChannelMessage("control", diagnostics_json);
        }
    }

