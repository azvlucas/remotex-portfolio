#pragma once

#include "host/screen_capture.h"

#include <mutex>

namespace remotex::host {

class CaptureCadenceController {
public:
    struct Counters {
        uint64_t capture_attempt_count = 0;
        uint64_t capture_hit_count = 0;
        uint64_t capture_timeout_count = 0;

        [[nodiscard]] double capture_hit_ratio() const {
            if (capture_attempt_count == 0) {
                return 0.0;
            }
            return static_cast<double>(capture_hit_count) /
                   static_cast<double>(capture_attempt_count);
        }
    };

    struct Snapshot {
        CapturedFrame frame;
        CaptureResult result;
    };

    void RecordAttempt();
    void RecordTimeout();
    void RecordHit(const CapturedFrame& frame, const CaptureResult& result);

    [[nodiscard]] Counters GetCounters() const;
    [[nodiscard]] bool TryGetLatestSnapshot(Snapshot& snapshot) const;

private:
    mutable std::mutex mutex_;
    Counters counters_;
    Snapshot latest_snapshot_;
    bool has_snapshot_ = false;
};

} // namespace remotex::host
