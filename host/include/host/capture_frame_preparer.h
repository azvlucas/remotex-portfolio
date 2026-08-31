#pragma once

#include "host/capture_damage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace remotex::host {

struct MappedFrameView {
    const uint8_t* data = nullptr;
    uint32_t row_pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FramePreparationResult {
    uint64_t copied_bytes = 0;
    double copy_ms = 0.0;
    double overlay_ms = 0.0;
    bool pointer_overlay_applied = false;
};

FramePreparationResult PrepareCaptureFrame(
    const CaptureDamageDecision& decision,
    const MappedFrameView& mapped,
    const CapturedFrame& frame,
    std::span<uint8_t> capture_bgra,
    uint32_t capture_pitch);

} // namespace remotex::host
