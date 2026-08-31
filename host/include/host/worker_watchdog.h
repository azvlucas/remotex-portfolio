#pragma once

namespace remotex::host {

struct WorkerWatchdogDecision {
    bool should_restart = false;
    bool heartbeat_stale = false;
    bool signaling_healthy = false;

    [[nodiscard]] bool deferred_for_signaling() const {
        return heartbeat_stale && signaling_healthy && !should_restart;
    }
};

inline WorkerWatchdogDecision EvaluateWorkerWatchdogDecision(bool heartbeat_healthy,
                                                             bool signaling_healthy) {
    WorkerWatchdogDecision decision;
    decision.heartbeat_stale = !heartbeat_healthy;
    decision.signaling_healthy = signaling_healthy;

    if (!heartbeat_healthy && signaling_healthy) {
        return decision;
    }

    decision.should_restart = !heartbeat_healthy || !signaling_healthy;
    return decision;
}

} // namespace remotex::host
