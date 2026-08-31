#include <gtest/gtest.h>

#include "core/video_encoder.h"

#include <chrono>

namespace remotex::core {
namespace {

void FeedViewerMetrics(ResolutionAdaptationState& state,
                       double avg_upload_ms,
                       double dropped_fps,
                       int count) {
    for (int i = 0; i < count; ++i) {
        state.UpdateViewerMetrics(avg_upload_ms, dropped_fps);
    }
}

} // namespace

TEST(ResolutionAdaptation, StartsAtFullResolution) {
    ResolutionAdaptationState state;

    EXPECT_EQ(state.GetCurrentStep(), 0);
    EXPECT_DOUBLE_EQ(state.GetScaleFactor(), 1.0);
}

TEST(ResolutionAdaptation, DownscalesWhenUploadMsHigh) {
    ResolutionAdaptationState state;
    const auto now = std::chrono::steady_clock::time_point{};

    FeedViewerMetrics(state, 15.0, 0.0, 4);

    EXPECT_TRUE(state.ShouldDownscale());
    state.Downscale(now);
    EXPECT_EQ(state.GetCurrentStep(), 1);
    EXPECT_DOUBLE_EQ(state.GetScaleFactor(), 0.75);
}

TEST(ResolutionAdaptation, DoesNotUpscaleBeforeHysteresis) {
    ResolutionAdaptationState state;
    const auto now = std::chrono::steady_clock::time_point{};

    FeedViewerMetrics(state, 15.0, 0.0, 4);
    state.Downscale(now);
    FeedViewerMetrics(state, 3.0, 0.0, 4);

    EXPECT_FALSE(state.ShouldUpscale(now + std::chrono::seconds(7)));
}

TEST(ResolutionAdaptation, UpscalesAfterHysteresis) {
    ResolutionAdaptationState state;
    const auto now = std::chrono::steady_clock::time_point{};

    FeedViewerMetrics(state, 15.0, 0.0, 4);
    state.Downscale(now);
    FeedViewerMetrics(state, 3.0, 0.0, 6);

    ASSERT_TRUE(state.ShouldUpscale(now + std::chrono::seconds(8)));
    state.Upscale(now + std::chrono::seconds(8));
    EXPECT_EQ(state.GetCurrentStep(), 0);
}

TEST(ResolutionAdaptation, ClampsAtMinStep) {
    ResolutionAdaptationState state;
    const auto now = std::chrono::steady_clock::time_point{};

    FeedViewerMetrics(state, 15.0, 0.0, 4);
    state.Downscale(now);
    FeedViewerMetrics(state, 15.0, 0.0, 4);
    state.Downscale(now + std::chrono::seconds(1));

    EXPECT_EQ(state.GetCurrentStep(), 2);
    EXPECT_FALSE(state.ShouldDownscale());
}

TEST(ResolutionAdaptation, DroppedFpsAcceleratesDownscale) {
    ResolutionAdaptationState state;

    FeedViewerMetrics(state, 10.0, 5.0, 4);

    EXPECT_TRUE(state.ShouldDownscale());
}

} // namespace remotex::core
