#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "host/screen_capture.h"

namespace remotex::host {

struct RecordingCanvas {
    int width = 0;
    int height = 0;
};

struct RecordingFrameLayout {
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
};

constexpr int RoundUpToEven(int value) {
    if (value <= 0) {
        return 0;
    }
    return (value % 2 == 0) ? value : (value + 1);
}

constexpr int RoundDownToEven(int value) {
    if (value <= 0) {
        return 0;
    }
    return (value % 2 == 0) ? value : (value - 1);
}

inline RecordingCanvas ComputeRecordingCanvas(std::span<const MonitorInfo> monitors,
                                              uint32_t fallback_width,
                                              uint32_t fallback_height) {
    int width = static_cast<int>(fallback_width);
    int height = static_cast<int>(fallback_height);

    for (const auto& monitor : monitors) {
        width = (std::max)(width, static_cast<int>(monitor.width));
        height = (std::max)(height, static_cast<int>(monitor.height));
    }

    return RecordingCanvas{
        .width = RoundUpToEven(width),
        .height = RoundUpToEven(height),
    };
}

inline RecordingFrameLayout ComputeRecordingFrameLayout(int source_width,
                                                        int source_height,
                                                        int canvas_width,
                                                        int canvas_height) {
    if (source_width <= 0 || source_height <= 0 || canvas_width <= 0 || canvas_height <= 0) {
        return {};
    }

    const double width_ratio = static_cast<double>(canvas_width) / static_cast<double>(source_width);
    const double height_ratio = static_cast<double>(canvas_height) / static_cast<double>(source_height);
    const double scale = (std::min)(1.0, (std::min)(width_ratio, height_ratio));

    int fitted_width = RoundDownToEven(static_cast<int>(std::floor(source_width * scale)));
    int fitted_height = RoundDownToEven(static_cast<int>(std::floor(source_height * scale)));

    if (fitted_width <= 0) {
        fitted_width = (std::min)(canvas_width, 2);
    }
    if (fitted_height <= 0) {
        fitted_height = (std::min)(canvas_height, 2);
    }

    fitted_width = (std::min)(fitted_width, RoundDownToEven(canvas_width));
    fitted_height = (std::min)(fitted_height, RoundDownToEven(canvas_height));

    int offset_x = RoundDownToEven((canvas_width - fitted_width) / 2);
    int offset_y = RoundDownToEven((canvas_height - fitted_height) / 2);

    return RecordingFrameLayout{
        .width = fitted_width,
        .height = fitted_height,
        .offset_x = (std::max)(0, offset_x),
        .offset_y = (std::max)(0, offset_y),
    };
}

} // namespace remotex::host
