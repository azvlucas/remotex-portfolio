#pragma once

#include "core/webrtc_peer.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace remotex::core {

struct FrameSlotData {
    std::vector<uint8_t> bgra_data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    VideoFrameMetrics metrics{};
    uint64_t frame_id = 0;
};

class BoundedFrameSlot {
public:
    bool TryEnqueue(FrameSlotData&& data);
    std::optional<FrameSlotData> TryDequeue();
    uint64_t GetDropCount() const;
    uint64_t ResetDropCount();

private:
    mutable std::mutex mutex_;
    std::optional<FrameSlotData> slot_;
    uint64_t drop_count_ = 0;
};

struct EncodePreset {
    const char* preset;
    int bitrate_factor_percent;
};

inline constexpr EncodePreset kLibx264Steps[] = {
    {"ultrafast", 100},
    {"ultrafast", 75},
    {"ultrafast", 50},
};

inline constexpr EncodePreset kNvencSteps[] = {
    {"p1", 100},
    {"p1", 75},
    {"p1", 50},
};

inline constexpr int kLibx264StepCount = 3;
inline constexpr int kNvencStepCount = 3;

class EncodeQualityState {
public:
    explicit EncodeQualityState(int max_steps);

    void UpdateEncodeTime(double sample_ms);
    bool ShouldStepDown() const;
    bool ShouldStepUp(std::chrono::steady_clock::time_point now) const;
    void StepDown(std::chrono::steady_clock::time_point now);
    void StepUp(std::chrono::steady_clock::time_point now);
    int GetCurrentStep() const;
    double GetRollingEncodeMs() const;

    static constexpr double kEwmaAlpha = 0.3;
    static constexpr double kStepDownThreshold = 28.0;
    static constexpr double kStepUpThreshold = 15.0;
    static constexpr auto kStepUpHysteresis = std::chrono::seconds(7);

private:
    int current_step_ = 0;
    int max_steps_ = 1;
    double rolling_encode_ms_ = 0.0;
    std::chrono::steady_clock::time_point last_step_change_{};
};

class ResolutionAdaptationState {
public:
    static constexpr double kSteps[] = {1.0, 0.75, 0.5};
    static constexpr int kStepCount = 3;
    static constexpr int kMinDimension = 320;

    static constexpr double kEwmaAlpha = 0.3;
    static constexpr double kDownscaleUploadMs = 12.0;
    static constexpr double kUpscaleUploadMs = 5.0;
    static constexpr double kDroppedFpsDownscaleBoost = 3.0;
    static constexpr auto kUpscaleHysteresis = std::chrono::seconds(8);

    void UpdateViewerMetrics(double avg_upload_ms, double dropped_fps);
    bool ShouldDownscale() const;
    bool ShouldUpscale(std::chrono::steady_clock::time_point now) const;
    void Downscale(std::chrono::steady_clock::time_point now);
    void Upscale(std::chrono::steady_clock::time_point now);
    int GetCurrentStep() const;
    double GetScaleFactor() const;
    double GetRollingUploadMs() const;
    void Reset();

private:
    int current_step_ = 0;
    double rolling_upload_ms_ = 0.0;
    double rolling_dropped_fps_ = 0.0;
    std::chrono::steady_clock::time_point last_step_change_{};
};

enum class SessionCodec {
    H264Nvenc,
    H264Libx264,
    FallbackI420,
};

} // namespace remotex::core
