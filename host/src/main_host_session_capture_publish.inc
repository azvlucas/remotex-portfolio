    // â”€â”€ Screen capture â†’ WebRTC â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void SendCaptureFrame() {
        std::string deferred_stage;
        std::string deferred_stage_detail;
        bool log_first_frame = false;
        bool first_frame_rotated_output = false;
        uint32_t first_frame_width = 0;
        uint32_t first_frame_height = 0;

        {
            std::lock_guard lock(capture_state_mutex_);
            if (capture_bgra_.empty() ||
                capture_bgra_width_ == 0 ||
                capture_bgra_height_ == 0 ||
                capture_bgra_pitch_ == 0 ||
                !latest_prepared_frame_) {
                capture_perf_.idle_skipped++;
                deferred_stage = "send_idle_no_frame";
                deferred_stage_detail = latest_prepared_frame_ ? "buffer-empty" : "no-prepared-frame";
            } else {
                auto frame_metrics = latest_prepared_frame_->metrics;
                const bool use_rotated_output = latest_prepared_frame_->uses_rotated_output;
                const uint8_t* frame_data = use_rotated_output
                    ? latest_prepared_frame_->output_buffer.data()
                    : capture_bgra_.data();
                const uint32_t frame_width = use_rotated_output
                    ? latest_prepared_frame_->output_width
                    : capture_bgra_width_;
                const uint32_t frame_height = use_rotated_output
                    ? latest_prepared_frame_->output_height
                    : capture_bgra_height_;
                const uint32_t frame_pitch = use_rotated_output
                    ? latest_prepared_frame_->output_pitch
                    : capture_bgra_pitch_;
                const size_t frame_size = use_rotated_output
                    ? latest_prepared_frame_->output_size
                    : capture_bgra_.size();
                if (frame_data == nullptr || frame_width == 0 || frame_height == 0 ||
                    frame_pitch == 0 || frame_size == 0) {
                    capture_perf_.idle_skipped++;
                    deferred_stage = "send_idle_no_frame";
                    deferred_stage_detail = "invalid-frame-buffer";
                } else {
                    if (latest_prepared_frame_->should_record &&
                        recorder_.IsRecording() &&
                        latest_prepared_frame_->frame_id != last_recorded_frame_id_) {
                        recorder_.PushFrame(frame_data,
                                            frame_size,
                                            frame_width,
                                            frame_height,
                                            frame_pitch);
                        last_recorded_frame_id_ = latest_prepared_frame_->frame_id;
                    }

                    frame_metrics.encode_ready_timestamp_us = 0;
                    capture_perf_.last_frame_id = latest_prepared_frame_->frame_id;

                    peer_.UpdateVideoDimensions(
                        static_cast<int>(frame_width),
                        static_cast<int>(frame_height));

                    const auto enqueue_start = std::chrono::steady_clock::now();
                    frame_metrics.send_started_timestamp_us = SteadyClockNowMicros();
                    peer_.SendVideoFrame(
                        frame_data,
                        frame_size,
                        frame_metrics.send_started_timestamp_us,
                        frame_metrics);
                    capture_perf_.send_ms +=
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - enqueue_start).count();
                    capture_perf_.encoded_frames++;
                    capture_perf_.sent_frames++;
                    last_frame_sent_time_ = std::chrono::steady_clock::now();

                    if (!first_frame_sent_) {
                        first_frame_sent_ = true;
                        deferred_stage = "frame_sent";
                        deferred_stage_detail = std::string("frame=") + std::to_string(frame_width) + "x" + std::to_string(frame_height);
                        log_first_frame = true;
                        first_frame_rotated_output = use_rotated_output;
                        first_frame_width = frame_width;
                        first_frame_height = frame_height;
                    }
                }
            }
        }

        if (!deferred_stage.empty()) {
            RecordSecureTransitionStage(deferred_stage.c_str(), deferred_stage_detail);
        }
        if (log_first_frame) {
            Log("First video frame sent to viewer (%s session, frame=%ux%u, rotated_output=%s)",
                IsWatchOnlySession() ? "watch-only" : "interactive",
                first_frame_width,
                first_frame_height,
                first_frame_rotated_output ? "yes" : "no");
        }
    }

    void ProcessCaptureAttempt(const remotex::host::CapturedFrame& frame,
                               const remotex::host::CaptureResult& capture_result,
                               const std::chrono::steady_clock::time_point capture_start,
                               const std::chrono::steady_clock::time_point capture_done) {
        capture_cadence_.RecordAttempt();
        {
            std::lock_guard lock(capture_state_mutex_);
            capture_perf_.capture_attempt_count++;
            capture_perf_.acquire_wait_ms += capture_result.acquire_wait_ms;
            RecordCaptureDiagnosticStateLocked(capture_result);
        }

        const auto status = capture_result.status;
        if (status == remotex::host::CaptureStatus::DisplayChanged) {
            RecordSecureTransitionStage("display_changed", "capture geometry/rotation changed");
            Log("Display geometry/rotation changed, rebuilding capture contract...");
            if (!capture_.Reinitialize()) {
                Log("ERROR: Failed to reinitialize capture after display change");
                MaybeLogCapturePerf();
                return;
            }

            {
                std::lock_guard lock(capture_state_mutex_);
                ResetCaptureFrameStateLocked();
                last_frame_sent_time_ = std::chrono::steady_clock::now();
                last_successful_capture_time_ = last_frame_sent_time_;
            }

            const auto& od = capture_.GetOutputDesc();
            injector_.SetCapturedMonitorBounds(
                od.DesktopCoordinates.left,
                od.DesktopCoordinates.top,
                od.DesktopCoordinates.right,
                od.DesktopCoordinates.bottom);
            SendInitialMonitorStatus();
            peer_.CleanupVideoEncoder();
            MaybeLogCapturePerf();
            return;
        }

        if (status == remotex::host::CaptureStatus::DesktopSwitched) {
            RecordSecureTransitionStage(
                "desktop_switched_detected",
                std::string("input=") + capture_result.input_desktop_name +
                    " thread=" + capture_result.thread_desktop_name,
                false,
                true);
            const auto now = std::chrono::steady_clock::now();
            if (now - last_capture_retry_ >= std::chrono::milliseconds(250)) {
                last_capture_retry_ = now;
                Log("Capture desktop transition detected, rebinding capture thread and rebuilding duplication...");
                LogCaptureDesktopContext("[capture_transition]", capture_result);
                capture_reinit_pending_.store(true);
            }
            MaybeLogCapturePerf();
            return;
        }

        if (status == remotex::host::CaptureStatus::DesktopContextUnavailable) {
            RecordSecureTransitionStage("desktop_context_unavailable",
                                        std::string("input_err=") + std::to_string(capture_result.input_desktop_error) + " thread_err=" + std::to_string(capture_result.thread_desktop_error),
                                        true);
            const auto now = std::chrono::steady_clock::now();
            if (now - last_capture_retry_ >= std::chrono::seconds(1)) {
                last_capture_retry_ = now;
                Log("Capture desktop context unavailable, retrying desktop bind + duplication init...");
                LogCaptureDesktopContext("[capture_context_unavailable]", capture_result);
                capture_reinit_pending_.store(true);
            }
            MaybeLogCapturePerf();
            return;
        }

        if (status == remotex::host::CaptureStatus::Timeout) {
            capture_cadence_.RecordTimeout();
            const auto now_timeout = std::chrono::steady_clock::now();
            bool reinitialize = false;
            {
                std::lock_guard lock(capture_state_mutex_);
                capture_perf_.timeout_capture_count++;
                reinitialize = first_frame_sent_ &&
                    (now_timeout - last_successful_capture_time_ >= std::chrono::seconds(10));
            }
            const bool secure_desktop_timeout =
                capture_result.input_desktop_available &&
                !capture_result.input_desktop_name.empty() &&
                _stricmp(capture_result.input_desktop_name.c_str(), "default") != 0 &&
                capture_result.input_matches_thread;
            if (secure_desktop_timeout &&
                now_timeout - last_successful_capture_time_ >= std::chrono::milliseconds(250) &&
                TryPrepareSecureDesktopFallbackFrame(capture_result, capture_done)) {
                MaybeLogCapturePerf();
                return;
            }
            RecordSecureTransitionStage(
                secure_desktop_timeout ? "secure_desktop_timeout" : "capture_timeout",
                std::string("input=") + capture_result.input_desktop_name +
                    " thread=" + capture_result.thread_desktop_name);
            if (reinitialize) {
                capture_.Reinitialize();
                {
                    std::lock_guard lock(capture_state_mutex_);
                    ResetCaptureFrameStateLocked();
                    first_frame_sent_ = false;
                    last_frame_sent_time_ = now_timeout;
                    last_successful_capture_time_ = now_timeout;
                }
                peer_.CleanupVideoEncoder();
            }
            MaybeLogCapturePerf();
            return;
        }

        if (status == remotex::host::CaptureStatus::DeviceLost) {
            Log("GPU device lost, reinitializing capture...");
            capture_.Reinitialize();
            {
                std::lock_guard lock(capture_state_mutex_);
                ResetCaptureFrameStateLocked();
            }
            peer_.CleanupVideoEncoder();
            MaybeLogCapturePerf();
            return;
        }

        if (status == remotex::host::CaptureStatus::AccessDenied) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_capture_retry_ >= std::chrono::seconds(2)) {
                last_capture_retry_ = now;
                Log("Capture access lost, retrying duplication init...");
                capture_.Reinitialize();
                {
                    std::lock_guard lock(capture_state_mutex_);
                    ResetCaptureFrameStateLocked();
                }
                peer_.CleanupVideoEncoder();
            }
            MaybeLogCapturePerf();
            return;
        }

        if (status != remotex::host::CaptureStatus::Ok || !frame.texture) {
            MaybeLogCapturePerf();
            return;
        }

        capture_cadence_.RecordHit(frame, capture_result);
        auto* ctx = capture_.GetContext();
        if (!ctx) {
            MaybeLogCapturePerf();
            return;
        }

        if (TryPrepareRotatedBootstrapFrameFromGdi(frame, capture_done)) {
            MaybeLogCapturePerf();
            return;
        }

        std::optional<std::string> mapped_frame_stage_detail;
        auto process_mapped_frame = [&](const D3D11_MAPPED_SUBRESOURCE& mapped) {
            std::lock_guard lock(capture_state_mutex_);
            last_successful_capture_time_ = capture_done;
            capture_perf_.capture_hit_count++;
            capture_perf_.captured_frames++;
            capture_perf_.accumulated_frames += capture_result.accumulated_frames;
            capture_perf_.dirty_rects += frame.dirty_rects.size();
            capture_perf_.move_rects += frame.move_rects.size();
            capture_perf_.capture_ms +=
                std::chrono::duration<double, std::milli>(capture_done - capture_start).count();

            const bool full_copy = EnsureCaptureBufferLocked(
                frame.width,
                frame.height,
                static_cast<uint32_t>(mapped.RowPitch));
            const DXGI_MODE_ROTATION rotation = capture_.GetRotation();
            const bool rotate_frame =
                rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                rotation == DXGI_MODE_ROTATION_ROTATE180 ||
                rotation == DXGI_MODE_ROTATION_ROTATE270;
            const bool force_first = !first_frame_sent_;
            auto decision = remotex::host::ClassifyCaptureDamage({
                .force_first_frame = force_first,
                .buffer_resized = full_copy,
                .frame = &frame,
                .previous_pointer = remotex::host::PointerSnapshot{
                    .region = last_pointer_region_,
                    .shape_info = last_pointer_shape_info_,
                    .visible = last_pointer_visible_,
                },
            });
            if (rotate_frame) {
                decision.overlay_pointer = false;
                if (rotated_bootstrap_full_refreshes_remaining_ > 0) {
                    decision.kind = remotex::host::CaptureUpdateKind::FullRefresh;
                    decision.copy_full_frame = true;
                    decision.replay_move_rects = false;
                    decision.copy_dirty_regions = false;
                    decision.copy_previous_pointer_region = false;
                    decision.copy_current_pointer_region = false;
                    decision.should_send = true;
                }
            }

            auto applied_decision = decision;
            auto preparation = remotex::host::PrepareCaptureFrame(
                applied_decision,
                remotex::host::MappedFrameView{
                    .data = static_cast<const uint8_t*>(mapped.pData),
                    .row_pitch = static_cast<uint32_t>(mapped.RowPitch),
                    .width = frame.width,
                    .height = frame.height,
                },
                frame,
                std::span<uint8_t>(capture_bgra_.data(), capture_bgra_.size()),
                capture_bgra_pitch_);
            capture_perf_.copied_bytes += preparation.copied_bytes;
            capture_perf_.copy_ms += preparation.copy_ms;
            capture_perf_.overlay_ms += preparation.overlay_ms;

            last_published_frame_id_ = frame.frame_index;
            auto frame_metrics = remotex::host::MakeVideoFrameMetrics(
                frame.frame_index,
                SteadyClockNowMicros());
            capture_perf_.last_frame_id = frame.frame_index;

            const bool visual_changed = decision.should_send;
            static constexpr auto kForceRefreshInterval = std::chrono::seconds(2);
            const auto now_send = std::chrono::steady_clock::now();
            const bool periodic_refresh = !visual_changed && first_frame_sent_ &&
                (now_send - last_frame_sent_time_ >= kForceRefreshInterval);

            if (periodic_refresh) {
                auto refresh_decision = decision;
                refresh_decision.kind = remotex::host::CaptureUpdateKind::FullRefresh;
                refresh_decision.copy_full_frame = true;
                refresh_decision.replay_move_rects = false;
                refresh_decision.copy_dirty_regions = false;
                refresh_decision.copy_previous_pointer_region = false;
                refresh_decision.copy_current_pointer_region = false;
                refresh_decision.overlay_pointer =
                    refresh_decision.current_pointer_region.has_value();
                refresh_decision.should_send = true;
                applied_decision = refresh_decision;

                preparation = remotex::host::PrepareCaptureFrame(
                    applied_decision,
                    remotex::host::MappedFrameView{
                        .data = static_cast<const uint8_t*>(mapped.pData),
                        .row_pitch = static_cast<uint32_t>(mapped.RowPitch),
                        .width = frame.width,
                        .height = frame.height,
                    },
                    frame,
                    std::span<uint8_t>(capture_bgra_.data(), capture_bgra_.size()),
                    capture_bgra_pitch_);
                capture_perf_.copied_bytes += preparation.copied_bytes;
                capture_perf_.copy_ms += preparation.copy_ms;
                capture_perf_.overlay_ms += preparation.overlay_ms;
            }

            PreparedCaptureFrame prepared_frame{
                .metrics = frame_metrics,
                .frame_id = frame.frame_index,
                .should_record = visual_changed || periodic_refresh,
                .uses_rotated_output = false,
                .output_width = capture_bgra_width_,
                .output_height = capture_bgra_height_,
                .output_pitch = capture_bgra_pitch_,
                .output_size = capture_bgra_.size(),
            };

            bool used_gdi_rotated_frame = false;
            if (rotate_frame) {
                const bool refresh_rotated_frame_from_gdi =
                    !first_frame_sent_ || visual_changed || periodic_refresh;
                if (refresh_rotated_frame_from_gdi) {
                    const auto& output_desc = capture_.GetOutputDesc();
                    const RECT region = output_desc.DesktopCoordinates;
                    remotex::host::GdiDesktopSnapshot snapshot;
                    DWORD gdi_error = ERROR_SUCCESS;
                    if (remotex::host::CaptureDesktopRegionGdi(region, snapshot, gdi_error) &&
                        snapshot.width > 0 &&
                        snapshot.height > 0 &&
                        !snapshot.bgra.empty()) {
                        prepared_frame.uses_rotated_output = true;
                        prepared_frame.output_width = snapshot.width;
                        prepared_frame.output_height = snapshot.height;
                        prepared_frame.output_pitch = snapshot.pitch;
                        prepared_frame.output_size = snapshot.bgra.size();
                        prepared_frame.output_buffer = std::move(snapshot.bgra);
                        prepared_frame.logical_dirty_regions.clear();
                        prepared_frame.logical_move_rects.clear();
                        prepared_frame.logical_pointer_region.reset();
                        prepared_frame.logical_pointer_position.reset();
                        prepared_frame.logical_dirty_regions.push_back(remotex::host::CopyRegion{
                            .left = 0,
                            .top = 0,
                            .right = static_cast<LONG>(prepared_frame.output_width),
                            .bottom = static_cast<LONG>(prepared_frame.output_height),
                        });

                        if (frame.pointer_visible) {
                            const LONG draw_height =
                                frame.pointer_shape_info.Type ==
                                        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
                                    ? static_cast<LONG>(frame.pointer_shape_info.Height / 2)
                                    : static_cast<LONG>(frame.pointer_shape_info.Height);
                            const POINT logical_pointer =
                                remotex::host::MapPointerDrawOriginIntoDesktopRegion(
                                    POINT{
                                        frame.pointer_x,
                                        frame.pointer_y,
                                    },
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

                            auto logical_pointer_region = remotex::host::ClipRegion(
                                remotex::host::CopyRegion{
                                    .left = logical_pointer.x,
                                    .top = logical_pointer.y,
                                    .right = logical_pointer.x +
                                        static_cast<LONG>(frame.pointer_shape_info.Width),
                                    .bottom = logical_pointer.y + draw_height,
                                },
                                prepared_frame.output_width,
                                prepared_frame.output_height);
                            if (remotex::host::IsValidRegion(logical_pointer_region)) {
                                prepared_frame.logical_pointer_region = logical_pointer_region;
                            }
                        }

                        capture_perf_.copied_bytes += prepared_frame.output_size;
                        rotated_bootstrap_full_refreshes_remaining_ = 0;
                        used_gdi_rotated_frame = true;
                    } else {
                        Log("WARNING: Rotated published frame GDI refresh failed: err=%lu region=[%ld,%ld,%ld,%ld] rotation=%d",
                            static_cast<unsigned long>(gdi_error),
                            region.left,
                            region.top,
                            region.right,
                            region.bottom,
                            static_cast<int>(rotation));
                    }
                }

                if (!used_gdi_rotated_frame) {
                    if (!frame_rotator_initialized_) {
                        frame_rotator_initialized_ =
                            frame_rotator_.Initialize(capture_.GetDevice());
                    }
                    if (!frame_rotator_initialized_ ||
                        !frame_rotator_.Rotate(
                            capture_bgra_.data(),
                            capture_bgra_width_,
                            capture_bgra_height_,
                            capture_bgra_pitch_,
                            rotation)) {
                        Log("ERROR: Failed to rotate prepared frame for rotation=%d",
                            static_cast<int>(rotation));
                        latest_prepared_frame_.reset();
                        return;
                    }

                    prepared_frame.uses_rotated_output = true;
                    prepared_frame.output_width = frame_rotator_.GetRotatedWidth();
                    prepared_frame.output_height = frame_rotator_.GetRotatedHeight();
                    prepared_frame.output_pitch = frame_rotator_.GetRotatedPitch();
                    prepared_frame.output_size =
                        static_cast<size_t>(prepared_frame.output_pitch) *
                        prepared_frame.output_height;
                    prepared_frame.output_buffer.assign(
                        frame_rotator_.GetRotatedData(),
                        frame_rotator_.GetRotatedData() + prepared_frame.output_size);

                    if (frame.pointer_visible) {
                        const LONG draw_height =
                            frame.pointer_shape_info.Type ==
                                    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
                                ? static_cast<LONG>(frame.pointer_shape_info.Height / 2)
                                : static_cast<LONG>(frame.pointer_shape_info.Height);
                        const POINT logical_pointer = remotex::host::RotatePointerDrawOrigin(
                            POINT{
                                frame.pointer_x,
                                frame.pointer_y,
                            },
                            frame.pointer_shape_info.HotSpot,
                            frame.pointer_shape_info.Width,
                            static_cast<uint32_t>(draw_height),
                            frame.width,
                            frame.height,
                            rotation);
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
                    }

                    const auto copied_regions =
                        remotex::host::CollectCopiedRegions(applied_decision, frame);
                    prepared_frame.logical_dirty_regions.reserve(copied_regions.size());
                    for (const auto& copied_region : copied_regions) {
                        prepared_frame.logical_dirty_regions.push_back(
                            remotex::host::RotateRegion(
                                copied_region,
                                frame.width,
                                frame.height,
                                rotation));
                    }

                    prepared_frame.logical_move_rects.reserve(frame.move_rects.size());
                    for (const auto& move_rect : frame.move_rects) {
                        prepared_frame.logical_move_rects.push_back(
                            remotex::host::RotateMoveRect(
                                move_rect,
                                frame.width,
                                frame.height,
                                rotation));
                    }

                    if (frame.pointer_visible &&
                        !frame.pointer_shape_buffer.empty() &&
                        frame.pointer_shape_info.Width > 0 &&
                        frame.pointer_shape_info.Height > 0) {
                        const LONG draw_height =
                            frame.pointer_shape_info.Type ==
                                    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
                                ? static_cast<LONG>(frame.pointer_shape_info.Height / 2)
                                : static_cast<LONG>(frame.pointer_shape_info.Height);
                        const POINT logical_pointer = prepared_frame.logical_pointer_position
                            .value_or(remotex::host::RotatePointerDrawOrigin(
                                POINT{
                                    frame.pointer_x,
                                    frame.pointer_y,
                                },
                                frame.pointer_shape_info.HotSpot,
                                frame.pointer_shape_info.Width,
                                static_cast<uint32_t>(draw_height),
                                frame.width,
                                frame.height,
                                rotation));
                        auto logical_pointer_region = remotex::host::ClipRegion(
                            remotex::host::CopyRegion{
                                .left = logical_pointer.x,
                                .top = logical_pointer.y,
                                .right = logical_pointer.x +
                                    static_cast<LONG>(frame.pointer_shape_info.Width),
                                .bottom = logical_pointer.y + draw_height,
                            },
                            prepared_frame.output_width,
                            prepared_frame.output_height);
                        if (remotex::host::IsValidRegion(logical_pointer_region)) {
                            prepared_frame.logical_pointer_region = logical_pointer_region;
                        }
                    }
                }

            }

            last_video_frame_metrics_ = frame_metrics;
            latest_prepared_frame_ = std::move(prepared_frame);
            mapped_frame_stage_detail = std::string("frame_id=") + std::to_string(frame.frame_index) +
                (used_gdi_rotated_frame ? " source=gdi" : "");
            if (rotate_frame &&
                applied_decision.copy_full_frame &&
                rotated_bootstrap_full_refreshes_remaining_ > 0) {
                --rotated_bootstrap_full_refreshes_remaining_;
            }
            last_pointer_region_ = applied_decision.current_pointer_region;
            last_pointer_visible_ = frame.pointer_visible;
            last_pointer_shape_info_ = frame.pointer_shape_info;
        };

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto map_start = std::chrono::steady_clock::now();
        HRESULT hr = ctx->Map(frame.texture.Get(),
                              0,
                              D3D11_MAP_READ,
                              0,
                              &mapped);
        if (SUCCEEDED(hr)) {
            {
                std::lock_guard lock(capture_state_mutex_);
                capture_perf_.map_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - map_start).count();
            }
            process_mapped_frame(mapped);
            ctx->Unmap(frame.texture.Get(), 0);
        }

        if (mapped_frame_stage_detail.has_value()) {
            RecordSecureTransitionStage(
                "prepared_frame_published",
                *mapped_frame_stage_detail,
                false,
                true);
        }

        MaybeLogCapturePerf();
    }
