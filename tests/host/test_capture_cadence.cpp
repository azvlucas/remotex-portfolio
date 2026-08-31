#include <gtest/gtest.h>

#include "host/capture_cadence.h"

namespace {

using remotex::host::CaptureCadenceController;
using remotex::host::CapturedFrame;
using remotex::host::CaptureResult;
using remotex::host::CaptureStatus;

TEST(CaptureCadence, TracksAttemptsHitsTimeoutsAndHitRatio) {
    CaptureCadenceController controller;
    controller.RecordAttempt();
    controller.RecordAttempt();
    controller.RecordTimeout();

    CapturedFrame frame;
    frame.frame_index = 41;
    CaptureResult result;
    result.status = CaptureStatus::Ok;
    result.acquire_wait_ms = 16.0;
    result.accumulated_frames = 2;
    controller.RecordAttempt();
    controller.RecordHit(frame, result);

    const auto counters = controller.GetCounters();
    EXPECT_EQ(counters.capture_attempt_count, 3u);
    EXPECT_EQ(counters.capture_hit_count, 1u);
    EXPECT_EQ(counters.capture_timeout_count, 1u);
    EXPECT_DOUBLE_EQ(counters.capture_hit_ratio(), 1.0 / 3.0);
}

TEST(CaptureCadence, ReturnsNewestSnapshotWithoutInferringFromOnePoll) {
    CaptureCadenceController controller;

    CapturedFrame older;
    older.frame_index = 10;
    CaptureResult older_result;
    older_result.status = CaptureStatus::Ok;
    older_result.acquire_wait_ms = 8.0;
    controller.RecordAttempt();
    controller.RecordHit(older, older_result);

    CapturedFrame newer;
    newer.frame_index = 11;
    CaptureResult newer_result;
    newer_result.status = CaptureStatus::Ok;
    newer_result.acquire_wait_ms = 12.0;
    newer_result.accumulated_frames = 3;
    controller.RecordAttempt();
    controller.RecordHit(newer, newer_result);

    CaptureCadenceController::Snapshot snapshot;
    ASSERT_TRUE(controller.TryGetLatestSnapshot(snapshot));
    EXPECT_EQ(snapshot.frame.frame_index, 11u);
    EXPECT_DOUBLE_EQ(snapshot.result.acquire_wait_ms, 12.0);
    EXPECT_EQ(snapshot.result.accumulated_frames, 3u);
}

} // namespace
