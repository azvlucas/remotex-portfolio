#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace remotex::host {

struct FinalizedRecordingResult {
    std::filesystem::path path;
    int duration_sec = 0;
    bool upload_dispatched = false;
};

inline int ComputeRecordingDurationSeconds(
    const std::chrono::steady_clock::time_point& started_at,
    const std::chrono::steady_clock::time_point& finished_at) {
    if (started_at.time_since_epoch() == std::chrono::steady_clock::duration::zero() ||
        finished_at <= started_at) {
        return 0;
    }

    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(finished_at - started_at).count());
}

template <typename StopFn, typename UploadFn>
FinalizedRecordingResult FinalizeRecordingIfPresent(
    bool is_recording,
    const std::chrono::steady_clock::time_point& started_at,
    const std::chrono::steady_clock::time_point& finished_at,
    const std::string& session_id,
    StopFn&& stop_fn,
    UploadFn&& upload_fn) {
    FinalizedRecordingResult result;
    if (!is_recording) {
        return result;
    }

    result.path = std::forward<StopFn>(stop_fn)();
    if (result.path.empty()) {
        return result;
    }

    result.duration_sec = ComputeRecordingDurationSeconds(started_at, finished_at);
    std::forward<UploadFn>(upload_fn)(result.path, session_id, result.duration_sec);
    result.upload_dispatched = true;
    return result;
}

} // namespace remotex::host
