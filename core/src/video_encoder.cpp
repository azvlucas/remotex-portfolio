#include "core/video_encoder.h"

#include <algorithm>
#include <utility>

namespace remotex::core {

bool BoundedFrameSlot::TryEnqueue(FrameSlotData&& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool dropped = slot_.has_value();
    if (dropped) {
        ++drop_count_;
    }
    slot_ = std::move(data);
    return dropped;
}

std::optional<FrameSlotData> BoundedFrameSlot::TryDequeue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!slot_.has_value()) {
        return std::nullopt;
    }

    std::optional<FrameSlotData> data = std::move(slot_);
    slot_.reset();
    return data;
}

uint64_t BoundedFrameSlot::GetDropCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drop_count_;
}

uint64_t BoundedFrameSlot::ResetDropCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t previous = drop_count_;
    drop_count_ = 0;
    return previous;
}

EncodeQualityState::EncodeQualityState(int max_steps)
    : max_steps_((std::max)(max_steps, 1)) {}

void EncodeQualityState::UpdateEncodeTime(double sample_ms) {
    if (rolling_encode_ms_ <= 0.0) {
        rolling_encode_ms_ = sample_ms;
        return;
    }

    rolling_encode_ms_ =
        (kEwmaAlpha * sample_ms) + ((1.0 - kEwmaAlpha) * rolling_encode_ms_);
}

bool EncodeQualityState::ShouldStepDown() const {
    return rolling_encode_ms_ >= kStepDownThreshold &&
           current_step_ < (max_steps_ - 1);
}

bool EncodeQualityState::ShouldStepUp(
    std::chrono::steady_clock::time_point now) const {
    return current_step_ > 0 &&
           rolling_encode_ms_ <= kStepUpThreshold &&
           (now - last_step_change_) >= kStepUpHysteresis;
}

void EncodeQualityState::StepDown(std::chrono::steady_clock::time_point now) {
    if (current_step_ >= (max_steps_ - 1)) {
        return;
    }

    ++current_step_;
    last_step_change_ = now;
}

void EncodeQualityState::StepUp(std::chrono::steady_clock::time_point now) {
    if (current_step_ <= 0) {
        return;
    }

    --current_step_;
    last_step_change_ = now;
}

int EncodeQualityState::GetCurrentStep() const {
    return current_step_;
}

double EncodeQualityState::GetRollingEncodeMs() const {
    return rolling_encode_ms_;
}

void ResolutionAdaptationState::UpdateViewerMetrics(double avg_upload_ms,
                                                    double dropped_fps) {
    if (rolling_upload_ms_ <= 0.0) {
        rolling_upload_ms_ = avg_upload_ms;
    } else {
        rolling_upload_ms_ =
            (kEwmaAlpha * avg_upload_ms) + ((1.0 - kEwmaAlpha) * rolling_upload_ms_);
    }

    if (rolling_dropped_fps_ <= 0.0) {
        rolling_dropped_fps_ = dropped_fps;
    } else {
        rolling_dropped_fps_ =
            (kEwmaAlpha * dropped_fps) + ((1.0 - kEwmaAlpha) * rolling_dropped_fps_);
    }
}

bool ResolutionAdaptationState::ShouldDownscale() const {
    if (current_step_ >= (kStepCount - 1)) {
        return false;
    }

    return rolling_upload_ms_ > kDownscaleUploadMs ||
           (rolling_upload_ms_ > (kDownscaleUploadMs - kDroppedFpsDownscaleBoost) &&
            rolling_dropped_fps_ > kDroppedFpsDownscaleBoost);
}

bool ResolutionAdaptationState::ShouldUpscale(
    std::chrono::steady_clock::time_point now) const {
    return current_step_ > 0 &&
           rolling_upload_ms_ < kUpscaleUploadMs &&
           (now - last_step_change_) >= kUpscaleHysteresis;
}

void ResolutionAdaptationState::Downscale(
    std::chrono::steady_clock::time_point now) {
    if (current_step_ >= (kStepCount - 1)) {
        return;
    }

    ++current_step_;
    rolling_upload_ms_ = 0.0;
    rolling_dropped_fps_ = 0.0;
    last_step_change_ = now;
}

void ResolutionAdaptationState::Upscale(
    std::chrono::steady_clock::time_point now) {
    if (current_step_ <= 0) {
        return;
    }

    --current_step_;
    rolling_upload_ms_ = 0.0;
    rolling_dropped_fps_ = 0.0;
    last_step_change_ = now;
}

int ResolutionAdaptationState::GetCurrentStep() const {
    return current_step_;
}

double ResolutionAdaptationState::GetScaleFactor() const {
    return kSteps[current_step_];
}

double ResolutionAdaptationState::GetRollingUploadMs() const {
    return rolling_upload_ms_;
}

void ResolutionAdaptationState::Reset() {
    current_step_ = 0;
    rolling_upload_ms_ = 0.0;
    rolling_dropped_fps_ = 0.0;
    last_step_change_ = {};
}

} // namespace remotex::core
