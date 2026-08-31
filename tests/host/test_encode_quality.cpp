#include <gtest/gtest.h>

#include "host/video_encoder.h"

#include <chrono>

namespace remotex::host {
namespace {

void FeedSamples(EncodeQualityState& state, double sample_ms, int count) {
    for (int i = 0; i < count; ++i) {
        state.UpdateEncodeTime(sample_ms);
    }
}

} // namespace

TEST(EncodeQualityState, InitialState) {
    EncodeQualityState state(3);

    EXPECT_EQ(state.GetCurrentStep(), 0);
    EXPECT_DOUBLE_EQ(state.GetRollingEncodeMs(), 0.0);
    EXPECT_FALSE(state.ShouldStepDown());
}

TEST(EncodeQualityState, StepDownWhenEncodeTimeTooHigh) {
    EncodeQualityState state(3);
    const auto now = std::chrono::steady_clock::now();

    FeedSamples(state, 35.0, 10);

    EXPECT_TRUE(state.ShouldStepDown());
    state.StepDown(now);
    EXPECT_EQ(state.GetCurrentStep(), 1);
}

TEST(EncodeQualityState, AtMaxStepCannotStepDown) {
    EncodeQualityState state(3);
    const auto now = std::chrono::steady_clock::now();

    FeedSamples(state, 35.0, 10);
    state.StepDown(now);
    state.StepDown(now);

    EXPECT_EQ(state.GetCurrentStep(), 2);
    EXPECT_FALSE(state.ShouldStepDown());
}

TEST(EncodeQualityState, AtStepZeroCannotStepUp) {
    EncodeQualityState state(3);
    const auto now = std::chrono::steady_clock::now();

    FeedSamples(state, 5.0, 20);

    EXPECT_FALSE(state.ShouldStepUp(now + std::chrono::seconds(100)));
}

TEST(EncodeQualityState, StepUpRequiresHysteresis) {
    EncodeQualityState state(3);
    const auto now = std::chrono::steady_clock::now();

    FeedSamples(state, 35.0, 10);
    state.StepDown(now);
    FeedSamples(state, 5.0, 20);

    EXPECT_FALSE(state.ShouldStepUp(now + std::chrono::seconds(3)));
    EXPECT_TRUE(state.ShouldStepUp(now + std::chrono::seconds(8)));
}

TEST(EncodeQualityState, StepUpDecrementsCurrentStep) {
    EncodeQualityState state(3);
    const auto now = std::chrono::steady_clock::now();

    FeedSamples(state, 35.0, 10);
    state.StepDown(now);
    FeedSamples(state, 5.0, 20);

    ASSERT_TRUE(state.ShouldStepUp(now + std::chrono::seconds(8)));
    state.StepUp(now + std::chrono::seconds(8));
    EXPECT_EQ(state.GetCurrentStep(), 0);
}

} // namespace remotex::host
