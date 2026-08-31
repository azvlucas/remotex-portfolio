#include <gtest/gtest.h>

#include <vector>

#include "host/recording_utils.h"

using namespace remotex::host;

TEST(RecordingUtils, ComputeRecordingCanvasUsesLargestMonitor) {
    MonitorInfo first{};
    first.width = 1920;
    first.height = 1080;

    MonitorInfo second{};
    second.width = 2560;
    second.height = 1440;

    std::vector<MonitorInfo> monitors = {first, second};

    const RecordingCanvas canvas = ComputeRecordingCanvas(monitors, 1280, 720);

    EXPECT_EQ(canvas.width, 2560);
    EXPECT_EQ(canvas.height, 1440);
}

TEST(RecordingUtils, ComputeRecordingFrameLayoutCentersSmallerInputWithoutUpscaling) {
    const RecordingFrameLayout layout = ComputeRecordingFrameLayout(1280, 720, 1920, 1080);

    EXPECT_EQ(layout.width, 1280);
    EXPECT_EQ(layout.height, 720);
    EXPECT_EQ(layout.offset_x, 320);
    EXPECT_EQ(layout.offset_y, 180);
}

TEST(RecordingUtils, ComputeRecordingFrameLayoutDownscalesToFitCanvas) {
    const RecordingFrameLayout layout = ComputeRecordingFrameLayout(2560, 1440, 1920, 1080);

    EXPECT_EQ(layout.width, 1920);
    EXPECT_EQ(layout.height, 1080);
    EXPECT_EQ(layout.offset_x, 0);
    EXPECT_EQ(layout.offset_y, 0);
}

TEST(RecordingUtils, ComputeRecordingFrameLayoutPreservesAspectRatioAcrossDifferentShapes) {
    const RecordingFrameLayout layout = ComputeRecordingFrameLayout(1920, 1200, 1920, 1080);

    EXPECT_EQ(layout.width, 1728);
    EXPECT_EQ(layout.height, 1080);
    EXPECT_EQ(layout.offset_x, 96);
    EXPECT_EQ(layout.offset_y, 0);
}
