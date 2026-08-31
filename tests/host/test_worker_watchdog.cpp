#include <gtest/gtest.h>

#include "host/worker_watchdog.h"

namespace {

using remotex::host::EvaluateWorkerWatchdogDecision;

TEST(WorkerWatchdog, KeepsHealthyWorkerRunning) {
    const auto decision = EvaluateWorkerWatchdogDecision(/*heartbeat_healthy=*/true,
                                                         /*signaling_healthy=*/true);

    EXPECT_FALSE(decision.should_restart);
    EXPECT_FALSE(decision.heartbeat_stale);
    EXPECT_TRUE(decision.signaling_healthy);
    EXPECT_FALSE(decision.deferred_for_signaling());
}

TEST(WorkerWatchdog, DefersRestartWhenHeartbeatIsStaleButSignalingIsFresh) {
    const auto decision = EvaluateWorkerWatchdogDecision(/*heartbeat_healthy=*/false,
                                                         /*signaling_healthy=*/true);

    EXPECT_FALSE(decision.should_restart);
    EXPECT_TRUE(decision.heartbeat_stale);
    EXPECT_TRUE(decision.signaling_healthy);
    EXPECT_TRUE(decision.deferred_for_signaling());
}

TEST(WorkerWatchdog, RestartsWhenSignalingIsUnhealthyEvenIfHeartbeatIsFresh) {
    const auto decision = EvaluateWorkerWatchdogDecision(/*heartbeat_healthy=*/true,
                                                         /*signaling_healthy=*/false);

    EXPECT_TRUE(decision.should_restart);
    EXPECT_FALSE(decision.heartbeat_stale);
    EXPECT_FALSE(decision.signaling_healthy);
    EXPECT_FALSE(decision.deferred_for_signaling());
}

TEST(WorkerWatchdog, RestartsWhenHeartbeatAndSignalingAreBothUnhealthy) {
    const auto decision = EvaluateWorkerWatchdogDecision(/*heartbeat_healthy=*/false,
                                                         /*signaling_healthy=*/false);

    EXPECT_TRUE(decision.should_restart);
    EXPECT_TRUE(decision.heartbeat_stale);
    EXPECT_FALSE(decision.signaling_healthy);
    EXPECT_FALSE(decision.deferred_for_signaling());
}

} // namespace
