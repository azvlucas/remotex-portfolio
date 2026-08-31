#include <gtest/gtest.h>

#include "viewer/remote_session_window.h"

namespace {

using remotex::viewer::SessionDiagnosticsBottleneck;
using remotex::viewer::SessionDiagnosticsSummary;

TEST(SessionDiagnostics, ClassifiesCaptureBoundWhenHostCaptureTrailing) {
    SessionDiagnosticsSummary summary;
    summary.host_capture_fps = 12.0;
    summary.host_encoded_fps = 29.0;
    summary.host_sent_fps = 29.0;
    summary.viewer_arrival_fps = 29.0;
    summary.viewer_decode_ready_fps = 29.0;
    summary.viewer_render_fps = 28.0;
    summary.viewer_upload_fps = 28.0;
    summary.frame_age_ms = 124.0;

    EXPECT_EQ(remotex::viewer::ClassifySessionDiagnostics(summary),
              SessionDiagnosticsBottleneck::Capture);
}

TEST(SessionDiagnostics, ClassifiesEncodeTransportWhenHostSendPathTrailing) {
    SessionDiagnosticsSummary summary;
    summary.host_capture_fps = 30.0;
    summary.host_encoded_fps = 17.0;
    summary.host_sent_fps = 16.0;
    summary.viewer_arrival_fps = 15.0;
    summary.viewer_decode_ready_fps = 15.0;
    summary.viewer_render_fps = 15.0;
    summary.viewer_upload_fps = 15.0;
    summary.frame_age_ms = 140.0;

    EXPECT_EQ(remotex::viewer::ClassifySessionDiagnostics(summary),
              SessionDiagnosticsBottleneck::EncodeTransport);
}

TEST(SessionDiagnostics, ClassifiesDecodeRenderWhenViewerDecodeTrailIsSlow) {
    SessionDiagnosticsSummary summary;
    summary.host_capture_fps = 30.0;
    summary.host_encoded_fps = 30.0;
    summary.host_sent_fps = 30.0;
    summary.viewer_arrival_fps = 30.0;
    summary.viewer_decode_ready_fps = 14.0;
    summary.viewer_render_fps = 13.0;
    summary.viewer_upload_fps = 13.0;
    summary.frame_age_ms = 150.0;

    EXPECT_EQ(remotex::viewer::ClassifySessionDiagnostics(summary),
              SessionDiagnosticsBottleneck::DecodeRender);
}

TEST(SessionDiagnostics, ClassifiesViewerBoundWhenUploadIsSlowestWithHighAge) {
    SessionDiagnosticsSummary summary;
    summary.host_capture_fps = 30.0;
    summary.host_encoded_fps = 30.0;
    summary.host_sent_fps = 30.0;
    summary.viewer_arrival_fps = 30.0;
    summary.viewer_decode_ready_fps = 30.0;
    summary.viewer_render_fps = 30.0;
    summary.viewer_upload_fps = 11.0;
    summary.avg_render_upload_ms = 18.0;
    summary.frame_age_ms = 170.0;

    EXPECT_EQ(remotex::viewer::ClassifySessionDiagnostics(summary),
              SessionDiagnosticsBottleneck::Viewer);
}

TEST(SessionDiagnostics, PrefersUnknownWhenFrameAgeIsHealthyAndStageRatesAlign) {
    SessionDiagnosticsSummary summary;
    summary.host_capture_fps = 29.5;
    summary.host_encoded_fps = 29.0;
    summary.host_sent_fps = 29.0;
    summary.viewer_arrival_fps = 29.0;
    summary.viewer_decode_ready_fps = 28.5;
    summary.viewer_render_fps = 28.0;
    summary.viewer_upload_fps = 28.0;
    summary.frame_age_ms = 18.0;

    EXPECT_EQ(remotex::viewer::ClassifySessionDiagnostics(summary),
              SessionDiagnosticsBottleneck::Unknown);
    EXPECT_EQ(remotex::viewer::SessionDiagnosticsBottleneckLabel(
                  SessionDiagnosticsBottleneck::EncodeTransport),
              QString("encode_transport"));
}

} // namespace
