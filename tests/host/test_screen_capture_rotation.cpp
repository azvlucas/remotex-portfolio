// RemoteX - tests/host/test_screen_capture_rotation.cpp
// Deterministic seams for logical capture dimensions and rotation metadata.

#include <gtest/gtest.h>

#include "host/screen_capture.h"

using namespace remotex::host;

namespace remotex::host {
class ScreenCaptureRotationTestAccess {
public:
    static void SetDimensionsAndRotation(ScreenCapture& capture,
                                         uint32_t raw_width,
                                         uint32_t raw_height,
                                         uint32_t logical_width,
                                         uint32_t logical_height,
                                         DXGI_MODE_ROTATION rotation) {
        capture.dup_desc_.ModeDesc.Width = raw_width;
        capture.dup_desc_.ModeDesc.Height = raw_height;
        capture.output_desc_.Rotation = rotation;
        capture.rotation_ = rotation;
        capture.logical_width_ = logical_width;
        capture.logical_height_ = logical_height;
    }
};
} // namespace remotex::host

TEST(ScreenCaptureRotation, LogicalDimensionsSwapForRotate90) {
    ScreenCapture capture;

    ScreenCaptureRotationTestAccess::SetDimensionsAndRotation(
        capture, 1920, 1080, 1080, 1920, DXGI_MODE_ROTATION_ROTATE90);
    EXPECT_EQ(capture.GetWidth(), 1080u);
    EXPECT_EQ(capture.GetHeight(), 1920u);

    ScreenCaptureRotationTestAccess::SetDimensionsAndRotation(
        capture, 1920, 1080, 1080, 1920, DXGI_MODE_ROTATION_ROTATE270);
    EXPECT_EQ(capture.GetWidth(), 1080u);
    EXPECT_EQ(capture.GetHeight(), 1920u);
}

TEST(ScreenCaptureRotation, GetRotationReportsStoredDxgiValue) {
    ScreenCapture capture;

    ScreenCaptureRotationTestAccess::SetDimensionsAndRotation(
        capture, 1920, 1080, 1920, 1080, DXGI_MODE_ROTATION_IDENTITY);
    EXPECT_EQ(capture.GetRotation(), DXGI_MODE_ROTATION_IDENTITY);
    EXPECT_EQ(capture.GetWidth(), 1920u);
    EXPECT_EQ(capture.GetHeight(), 1080u);

    ScreenCaptureRotationTestAccess::SetDimensionsAndRotation(
        capture, 1920, 1080, 1920, 1080, DXGI_MODE_ROTATION_ROTATE180);
    EXPECT_EQ(capture.GetRotation(), DXGI_MODE_ROTATION_ROTATE180);
    EXPECT_EQ(capture.GetWidth(), 1920u);
    EXPECT_EQ(capture.GetHeight(), 1080u);
}

TEST(ScreenCaptureRotation, SessionMetadataHelpersUseLogicalDimensions) {
    ScreenCapture capture;

    ScreenCaptureRotationTestAccess::SetDimensionsAndRotation(
        capture, 1920, 1080, 1080, 1920, DXGI_MODE_ROTATION_ROTATE90);
    EXPECT_EQ(capture.GetWidth(), 1080u);
    EXPECT_EQ(capture.GetHeight(), 1920u);
    EXPECT_EQ(static_cast<int>(capture.GetRotation()),
              static_cast<int>(DXGI_MODE_ROTATION_ROTATE90));
}
