#include <gtest/gtest.h>

namespace {

double ComputeDroppedFps(unsigned long long submitted,
                         unsigned long long uploaded,
                         double elapsed_s) {
    return elapsed_s > 0.0
        ? static_cast<double>(submitted - uploaded) / elapsed_s
        : 0.0;
}

} // namespace

TEST(FrameDropCounting, NoDropsWhenUploadMatchesSubmit) {
    EXPECT_DOUBLE_EQ(ComputeDroppedFps(30, 30, 1.0), 0.0);
}

TEST(FrameDropCounting, DroppedFpsComputed) {
    EXPECT_DOUBLE_EQ(ComputeDroppedFps(35, 28, 1.0), 7.0);
}

TEST(FrameDropCounting, DroppedFpsScalesWithElapsed) {
    EXPECT_DOUBLE_EQ(ComputeDroppedFps(70, 56, 2.0), 7.0);
}
