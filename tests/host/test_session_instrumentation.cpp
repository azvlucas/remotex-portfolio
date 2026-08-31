#include <gtest/gtest.h>

#include "host/session_instrumentation.h"

namespace {

using remotex::core::VideoFrameMetrics;
using remotex::host::BuildSessionDiagnosticsJson;
using remotex::host::MakeVideoFrameMetrics;
using remotex::host::SessionInstrumentationSummary;
using remotex::host::SessionInstrumentationWindow;
using remotex::host::SummarizeInstrumentationWindow;

TEST(SessionInstrumentation, SummarizesRollingWindowAndEncodedThroughput) {
    SessionInstrumentationWindow window;
    window.window_seconds = 5.0;
    window.capture_attempt_count = 75;
    window.capture_hit_count = 60;
    window.captured_frames = 60;
    window.encoded_frames = 50;
    window.sent_frames = 45;
    window.timeout_capture_count = 15;
    window.idle_skipped = 7;
    window.copied_bytes = 25ull * 1024ull * 1024ull;
    window.dirty_rects = 90;
    window.move_rects = 15;
    window.last_frame_id = 1234;
    window.desktop_switch_count = 3;
    window.desktop_context_unavailable_count = 2;
    window.fallback_snapshot_success_count = 4;
    window.fallback_snapshot_failure_count = 1;
    window.quality_level = 2;
    window.acquire_wait_ms = 300.0;
    window.accumulated_frames = 120.0;
    window.capture_ms = 180.0;
    window.encode_ms = 250.0;
    window.map_ms = 120.0;
    window.copy_ms = 90.0;
    window.overlay_ms = 30.0;
    window.send_ms = 135.0;
    window.dropped_frames = 6;
    window.seconds_since_last_successful_capture = 1.75;
    window.input_matches_thread = false;
    window.secure_transition_failure = false;
    window.last_capture_status = "desktop_switched";
    window.previous_input_desktop_name = "default";
    window.input_desktop_name = "Winlogon";
    window.thread_desktop_name = "default";
    window.secure_transition_stage = "send_idle_no_frame";
    window.secure_transition_detail = "no-prepared-frame";
    window.last_successful_transition_stage = "prepared_frame_published";
    window.last_successful_transition_detail = "frame_id=1234";
    window.last_failed_transition_stage = "fallback_snapshot_failed";
    window.last_failed_transition_reason = "desktop=Winlogon err=5";

    const auto summary = SummarizeInstrumentationWindow(window);

    EXPECT_DOUBLE_EQ(summary.capture_fps, 12.0);
    EXPECT_DOUBLE_EQ(summary.capture_attempt_fps, 15.0);
    EXPECT_DOUBLE_EQ(summary.capture_hit_ratio, 0.8);
    EXPECT_DOUBLE_EQ(summary.encoded_fps, 10.0);
    EXPECT_DOUBLE_EQ(summary.sent_fps, 9.0);
    EXPECT_DOUBLE_EQ(summary.avg_acquire_wait_ms, 4.0);
    EXPECT_EQ(summary.timeout_capture_count, 15u);
    EXPECT_DOUBLE_EQ(summary.avg_accumulated_frames, 2.0);
    EXPECT_EQ(summary.idle_skipped, 7u);
    EXPECT_DOUBLE_EQ(summary.copy_mb_s, 5.0);
    EXPECT_DOUBLE_EQ(summary.avg_capture_ms, 3.0);
    EXPECT_DOUBLE_EQ(summary.avg_encode_ms, 5.0);
    EXPECT_DOUBLE_EQ(summary.avg_map_ms, 2.0);
    EXPECT_DOUBLE_EQ(summary.avg_copy_ms, 1.5);
    EXPECT_DOUBLE_EQ(summary.avg_overlay_ms, 0.5);
    EXPECT_DOUBLE_EQ(summary.avg_send_ms, 3.0);
    EXPECT_DOUBLE_EQ(summary.avg_dirty_rects, 1.5);
    EXPECT_DOUBLE_EQ(summary.avg_move_rects, 0.25);
    EXPECT_EQ(summary.last_frame_id, 1234u);
    EXPECT_EQ(summary.desktop_switch_count, 3u);
    EXPECT_EQ(summary.desktop_context_unavailable_count, 2u);
    EXPECT_EQ(summary.fallback_snapshot_success_count, 4u);
    EXPECT_EQ(summary.fallback_snapshot_failure_count, 1u);
    EXPECT_EQ(summary.dropped_frames, 6u);
    EXPECT_EQ(summary.quality_level, 2);
    EXPECT_DOUBLE_EQ(summary.seconds_since_last_successful_capture, 1.75);
    EXPECT_FALSE(summary.input_matches_thread);
    EXPECT_FALSE(summary.secure_transition_failure);
    EXPECT_EQ(summary.last_capture_status, "desktop_switched");
    EXPECT_EQ(summary.previous_input_desktop_name, "default");
    EXPECT_EQ(summary.input_desktop_name, "Winlogon");
    EXPECT_EQ(summary.thread_desktop_name, "default");
    EXPECT_EQ(summary.secure_transition_stage, "send_idle_no_frame");
    EXPECT_EQ(summary.secure_transition_detail, "no-prepared-frame");
    EXPECT_EQ(summary.last_successful_transition_stage, "prepared_frame_published");
    EXPECT_EQ(summary.last_successful_transition_detail, "frame_id=1234");
    EXPECT_EQ(summary.last_failed_transition_stage, "fallback_snapshot_failed");
    EXPECT_EQ(summary.last_failed_transition_reason, "desktop=Winlogon err=5");
}

TEST(SessionInstrumentation, BuildsDiagnosticsPayloadWithDesktopRecoveryState) {
    SessionInstrumentationWindow window;
    window.window_seconds = 4.0;
    window.capture_attempt_count = 52;
    window.capture_hit_count = 48;
    window.captured_frames = 48;
    window.encoded_frames = 36;
    window.sent_frames = 32;
    window.timeout_capture_count = 8;
    window.acquire_wait_ms = 224.0;
    window.accumulated_frames = 72.0;
    window.dropped_frames = 4;
    window.quality_level = 1;
    window.desktop_switch_count = 5;
    window.desktop_context_unavailable_count = 1;
    window.fallback_snapshot_success_count = 7;
    window.fallback_snapshot_failure_count = 2;
    window.seconds_since_last_successful_capture = 2.5;
    window.input_matches_thread = false;
    window.secure_transition_failure = true;
    window.last_capture_status = "desktop_switched";
    window.previous_input_desktop_name = "default";
    window.input_desktop_name = "Winlogon";
    window.thread_desktop_name = "default";
    window.secure_transition_stage = "capture_reinit_failed";
    window.secure_transition_detail = "capture_.Reinitialize returned false";
    window.last_successful_transition_stage = "transition_bootstrap_armed";
    window.last_successful_transition_detail = "reason=secure_attention";
    window.last_failed_transition_stage = "capture_reinit_failed";
    window.last_failed_transition_reason = "capture_.Reinitialize returned false";

    const auto summary = SummarizeInstrumentationWindow(window);
    const auto diagnostics_json = BuildSessionDiagnosticsJson(summary);

    EXPECT_NE(diagnostics_json.find("\"type\":\"session_diagnostics\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"capture_fps\":12.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"capture_attempt_fps\":13.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"capture_hit_ratio\":0.923"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"encoded_fps\":9.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"sent_fps\":8.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"avg_acquire_wait_ms\":4.31"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"timeout_capture_count\":8"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"avg_accumulated_frames\":1.5"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"dropped_frames\":4"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"quality_level\":1"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"desktop_switch_count\":5"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"desktop_context_unavailable_count\":1"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"fallback_snapshot_success_count\":7"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"fallback_snapshot_failure_count\":2"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"seconds_since_last_successful_capture\":2.50"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"input_matches_thread\":false"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_capture_status\":\"desktop_switched\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"previous_input_desktop_name\":\"default\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"input_desktop_name\":\"Winlogon\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"thread_desktop_name\":\"default\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"secure_transition_stage\":\"capture_reinit_failed\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"secure_transition_detail\":\"capture_.Reinitialize returned false\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"secure_transition_failure\":true"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_successful_transition_stage\":\"transition_bootstrap_armed\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_successful_transition_detail\":\"reason=secure_attention\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_failed_transition_stage\":\"capture_reinit_failed\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_failed_transition_reason\":\"capture_.Reinitialize returned false\""), std::string::npos);
}

TEST(SessionInstrumentation, EscapesTransitionDetailFieldsInDiagnosticsJson) {
    SessionInstrumentationSummary summary;
    summary.secure_transition_stage = "capture_reinit_failed";
    summary.secure_transition_detail = "path=\"secure\"\\retry";
    summary.secure_transition_failure = true;
    summary.last_successful_transition_stage = "prepared_frame_published";
    summary.last_successful_transition_detail = "frame_id=42";
    summary.last_failed_transition_stage = "capture_reinit_failed";
    summary.last_failed_transition_reason = "path=\"secure\"\\retry";

    const auto diagnostics_json = BuildSessionDiagnosticsJson(summary);

    EXPECT_NE(diagnostics_json.find("\"secure_transition_detail\":\"path=\\\"secure\\\"\\\\retry\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_failed_transition_reason\":\"path=\\\"secure\\\"\\\\retry\""), std::string::npos);
}

TEST(SessionInstrumentation, MakesVideoFrameMetricsForPropagationSeams) {
    const VideoFrameMetrics metrics = MakeVideoFrameMetrics(77, 1010, 2020, 3030);

    EXPECT_EQ(metrics.frame_id, 77u);
    EXPECT_EQ(metrics.capture_ready_timestamp_us, 1010);
    EXPECT_EQ(metrics.encode_ready_timestamp_us, 2020);
    EXPECT_EQ(metrics.send_started_timestamp_us, 3030);
}

TEST(SessionInstrumentation, DiagnosticsJsonIncludesDroppedFramesQualityAndRecoveryFields) {
    SessionInstrumentationSummary summary;
    summary.capture_fps = 30.0;
    summary.capture_attempt_fps = 31.0;
    summary.capture_hit_ratio = 0.95;
    summary.encoded_fps = 29.0;
    summary.sent_fps = 28.0;
    summary.avg_acquire_wait_ms = 2.5;
    summary.timeout_capture_count = 1;
    summary.avg_accumulated_frames = 1.2;
    summary.dropped_frames = 9;
    summary.quality_level = 2;
    summary.desktop_switch_count = 2;
    summary.desktop_context_unavailable_count = 1;
    summary.fallback_snapshot_success_count = 3;
    summary.fallback_snapshot_failure_count = 1;
    summary.seconds_since_last_successful_capture = 0.75;
    summary.input_matches_thread = true;
    summary.secure_transition_failure = false;
    summary.last_capture_status = "ok";
    summary.previous_input_desktop_name = "default";
    summary.input_desktop_name = "default";
    summary.thread_desktop_name = "default";
    summary.secure_transition_stage = "prepared_frame_published";
    summary.secure_transition_detail = "frame_id=88";
    summary.last_successful_transition_stage = "prepared_frame_published";
    summary.last_successful_transition_detail = "frame_id=88";
    summary.last_failed_transition_stage = "fallback_snapshot_failed";
    summary.last_failed_transition_reason = "desktop=Winlogon err=5";

    const auto diagnostics_json = BuildSessionDiagnosticsJson(summary);

    EXPECT_NE(diagnostics_json.find("\"dropped_frames\":9"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"quality_level\":2"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"desktop_switch_count\":2"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"desktop_context_unavailable_count\":1"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"fallback_snapshot_success_count\":3"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"fallback_snapshot_failure_count\":1"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"seconds_since_last_successful_capture\":0.75"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"input_matches_thread\":true"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_capture_status\":\"ok\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"secure_transition_stage\":\"prepared_frame_published\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_successful_transition_stage\":\"prepared_frame_published\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_failed_transition_stage\":\"fallback_snapshot_failed\""), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"last_failed_transition_reason\":\"desktop=Winlogon err=5\""), std::string::npos);
}

} // namespace
