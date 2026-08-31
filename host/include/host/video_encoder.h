#pragma once

// Video encoder types moved to core/ — this header forwards for backward compatibility.
#include "core/video_encoder.h"

namespace remotex::host {
    using remotex::core::FrameSlotData;
    using remotex::core::BoundedFrameSlot;
    using remotex::core::EncodePreset;
    using remotex::core::kLibx264Steps;
    using remotex::core::kNvencSteps;
    using remotex::core::kLibx264StepCount;
    using remotex::core::kNvencStepCount;
    using remotex::core::EncodeQualityState;
    using remotex::core::SessionCodec;
} // namespace remotex::host
