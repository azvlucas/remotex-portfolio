#include "host/capture_cadence.h"

namespace remotex::host {

void CaptureCadenceController::RecordAttempt() {
    std::lock_guard lock(mutex_);
    ++counters_.capture_attempt_count;
}

void CaptureCadenceController::RecordTimeout() {
    std::lock_guard lock(mutex_);
    ++counters_.capture_timeout_count;
}

void CaptureCadenceController::RecordHit(const CapturedFrame& frame,
                                         const CaptureResult& result) {
    std::lock_guard lock(mutex_);
    ++counters_.capture_hit_count;
    latest_snapshot_.frame = frame;
    latest_snapshot_.result = result;
    has_snapshot_ = true;
}

CaptureCadenceController::Counters CaptureCadenceController::GetCounters() const {
    std::lock_guard lock(mutex_);
    return counters_;
}

bool CaptureCadenceController::TryGetLatestSnapshot(Snapshot& snapshot) const {
    std::lock_guard lock(mutex_);
    if (!has_snapshot_) {
        return false;
    }
    snapshot = latest_snapshot_;
    return true;
}

} // namespace remotex::host
