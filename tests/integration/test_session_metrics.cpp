#include <gtest/gtest.h>

#include "host/session_instrumentation.h"
#include "viewer/remote_session_window.h"

namespace {

struct ControlledCaptureEvidence {
    double capture_fps = 0.0;
    double capture_attempt_fps = 0.0;
    double capture_hit_ratio = 0.0;
    double encoded_fps = 0.0;
    double sent_fps = 0.0;
    double avg_acquire_wait_ms = 0.0;
    uint64_t timeout_capture_count = 0;
    double avg_accumulated_frames = 0.0;
    double frame_age_ms = 0.0;
    bool meets_30_fps_baseline = false;
    bool capture_limited_by_acquisition = false;
    remotex::viewer::SessionDiagnosticsBottleneck bottleneck =
        remotex::viewer::SessionDiagnosticsBottleneck::Unknown;
};

ControlledCaptureEvidence BuildControlledCaptureEvidence(
    const remotex::host::SessionInstrumentationSummary& host_summary,
    const remotex::viewer::SessionDiagnosticsSummary& viewer_summary) {
    ControlledCaptureEvidence evidence;
    evidence.capture_fps = host_summary.capture_fps;
    evidence.capture_attempt_fps = host_summary.capture_attempt_fps;
    evidence.capture_hit_ratio = host_summary.capture_hit_ratio;
    evidence.encoded_fps = host_summary.encoded_fps;
    evidence.sent_fps = host_summary.sent_fps;
    evidence.avg_acquire_wait_ms = host_summary.avg_acquire_wait_ms;
    evidence.timeout_capture_count = host_summary.timeout_capture_count;
    evidence.avg_accumulated_frames = host_summary.avg_accumulated_frames;
    evidence.frame_age_ms = viewer_summary.frame_age_ms;
    evidence.meets_30_fps_baseline = host_summary.capture_fps >= 30.0;
    evidence.capture_limited_by_acquisition =
        host_summary.capture_attempt_fps >= host_summary.sent_fps &&
        host_summary.capture_hit_ratio < 0.95 &&
        host_summary.capture_fps < 30.0 &&
        host_summary.avg_acquire_wait_ms > 0.0;
    evidence.bottleneck = remotex::viewer::ClassifySessionDiagnostics(viewer_summary);
    return evidence;
}

TEST(SessionMetrics, ControlledLanPathPreservesCorrelatedStageEvidence) {
    remotex::host::SessionInstrumentationWindow host_window;
    host_window.window_seconds = 5.0;
    host_window.capture_attempt_count = 180;
    host_window.capture_hit_count = 150;
    host_window.captured_frames = 150;
    host_window.timeout_capture_count = 30;
    host_window.encoded_frames = 110;
    host_window.sent_frames = 105;
    host_window.acquire_wait_ms = 720.0;
    host_window.accumulated_frames = 210.0;
    const auto host_summary = remotex::host::SummarizeInstrumentationWindow(host_window);

    remotex::viewer::SessionDiagnosticsSummary viewer_summary;
    viewer_summary.host_capture_fps = host_summary.capture_fps;
    viewer_summary.host_encoded_fps = host_summary.encoded_fps;
    viewer_summary.host_sent_fps = host_summary.sent_fps;
    viewer_summary.viewer_arrival_fps = 20.0;
    viewer_summary.viewer_decode_ready_fps = 20.0;
    viewer_summary.viewer_render_fps = 19.5;
    viewer_summary.viewer_upload_fps = 19.5;
    viewer_summary.avg_transport_ms = 26.0;
    viewer_summary.avg_decode_queue_ms = 5.0;
    viewer_summary.avg_render_upload_ms = 4.0;
    viewer_summary.frame_age_ms = 132.0;
    viewer_summary.frame_age_is_end_to_end = true;

    const auto evidence = BuildControlledCaptureEvidence(host_summary, viewer_summary);
    EXPECT_DOUBLE_EQ(evidence.capture_fps, 30.0);
    EXPECT_DOUBLE_EQ(evidence.capture_attempt_fps, 36.0);
    EXPECT_DOUBLE_EQ(evidence.capture_hit_ratio, 150.0 / 180.0);
    EXPECT_DOUBLE_EQ(evidence.encoded_fps, 22.0);
    EXPECT_DOUBLE_EQ(evidence.sent_fps, 21.0);
    EXPECT_DOUBLE_EQ(evidence.avg_acquire_wait_ms, 4.0);
    EXPECT_EQ(evidence.timeout_capture_count, 30u);
    EXPECT_DOUBLE_EQ(evidence.avg_accumulated_frames, 1.4);
    EXPECT_DOUBLE_EQ(evidence.frame_age_ms, 132.0);
    EXPECT_TRUE(evidence.meets_30_fps_baseline);
    EXPECT_FALSE(evidence.capture_limited_by_acquisition);
    EXPECT_EQ(evidence.bottleneck,
              remotex::viewer::SessionDiagnosticsBottleneck::EncodeTransport)
        << "capture=" << viewer_summary.host_capture_fps
        << " encoded=" << viewer_summary.host_encoded_fps
        << " sent=" << viewer_summary.host_sent_fps
        << " arrival=" << viewer_summary.viewer_arrival_fps
        << " frame_age=" << viewer_summary.frame_age_ms;
}

TEST(SessionMetrics, DecodeEvidenceStaysSeparateFromTransport) {
    remotex::viewer::SessionDiagnosticsSummary viewer_summary;
    viewer_summary.host_capture_fps = 30.0;
    viewer_summary.host_encoded_fps = 30.0;
    viewer_summary.host_sent_fps = 30.0;
    viewer_summary.viewer_arrival_fps = 29.5;
    viewer_summary.viewer_decode_ready_fps = 13.0;
    viewer_summary.viewer_render_fps = 12.0;
    viewer_summary.viewer_upload_fps = 12.0;
    viewer_summary.avg_transport_ms = 11.0;
    viewer_summary.avg_decode_queue_ms = 34.0;
    viewer_summary.avg_render_upload_ms = 7.0;
    viewer_summary.frame_age_ms = 165.0;
    viewer_summary.frame_age_is_end_to_end = true;

    EXPECT_EQ(remotex::viewer::ClassifySessionDiagnostics(viewer_summary),
              remotex::viewer::SessionDiagnosticsBottleneck::DecodeRender);
}

TEST(SessionMetrics, DiagnosticsPayloadCarriesEncodedFpsForControlledRuns) {
    remotex::host::SessionInstrumentationWindow host_window;
    host_window.window_seconds = 3.0;
    host_window.capture_attempt_count = 102;
    host_window.capture_hit_count = 90;
    host_window.captured_frames = 90;
    host_window.timeout_capture_count = 12;
    host_window.encoded_frames = 87;
    host_window.sent_frames = 84;
    host_window.acquire_wait_ms = 408.0;
    host_window.accumulated_frames = 126.0;

    const auto host_summary = remotex::host::SummarizeInstrumentationWindow(host_window);
    const auto diagnostics_json = remotex::host::BuildSessionDiagnosticsJson(host_summary);

    EXPECT_NE(diagnostics_json.find("\"encoded_fps\":29.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"sent_fps\":28.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"capture_attempt_fps\":34.0"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"capture_hit_ratio\":0.882"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"avg_acquire_wait_ms\":4.00"), std::string::npos);
    EXPECT_NE(diagnostics_json.find("\"timeout_capture_count\":12"), std::string::npos);
}

TEST(SessionMetrics, AcquisitionEvidenceFlagsCaptureStarvationSeparately) {
    remotex::host::SessionInstrumentationWindow host_window;
    host_window.window_seconds = 5.0;
    host_window.capture_attempt_count = 150;
    host_window.capture_hit_count = 54;
    host_window.captured_frames = 54;
    host_window.timeout_capture_count = 96;
    host_window.encoded_frames = 54;
    host_window.sent_frames = 54;
    host_window.acquire_wait_ms = 600.0;
    host_window.accumulated_frames = 70.0;

    remotex::viewer::SessionDiagnosticsSummary viewer_summary;
    viewer_summary.host_capture_fps = 10.8;
    viewer_summary.host_encoded_fps = 10.8;
    viewer_summary.host_sent_fps = 10.8;
    viewer_summary.viewer_arrival_fps = 10.8;
    viewer_summary.viewer_decode_ready_fps = 10.8;
    viewer_summary.viewer_render_fps = 10.8;
    viewer_summary.viewer_upload_fps = 10.8;
    viewer_summary.frame_age_ms = 28.0;

    const auto evidence = BuildControlledCaptureEvidence(
        remotex::host::SummarizeInstrumentationWindow(host_window),
        viewer_summary);

    EXPECT_FALSE(evidence.meets_30_fps_baseline);
    EXPECT_TRUE(evidence.capture_limited_by_acquisition);
    EXPECT_DOUBLE_EQ(evidence.capture_attempt_fps, 30.0);
    EXPECT_DOUBLE_EQ(evidence.capture_hit_ratio, 54.0 / 150.0);
    EXPECT_EQ(evidence.timeout_capture_count, 96u);
    EXPECT_DOUBLE_EQ(evidence.avg_acquire_wait_ms, 4.0);
}

} // namespace
