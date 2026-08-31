#include "host/capture_frame_preparer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace remotex::host {

namespace {

void CopyRegionFromMapped(std::span<uint8_t> capture_bgra,
                          uint32_t capture_pitch,
                          const MappedFrameView& mapped,
                          const CopyRegion& region,
                          uint64_t& copied_bytes) {
    const CopyRegion clipped = ClipRegion(region, mapped.width, mapped.height);
    if (!IsValidRegion(clipped) || capture_bgra.empty() || mapped.data == nullptr) {
        return;
    }

    const size_t row_bytes = static_cast<size_t>(clipped.right - clipped.left) * 4u;
    auto* dst_base = capture_bgra.data();

    for (LONG y = clipped.top; y < clipped.bottom; ++y) {
        const size_t src_offset = static_cast<size_t>(y) * mapped.row_pitch +
                                  static_cast<size_t>(clipped.left) * 4u;
        const size_t dst_offset = static_cast<size_t>(y) * capture_pitch +
                                  static_cast<size_t>(clipped.left) * 4u;
        std::memcpy(dst_base + dst_offset, mapped.data + src_offset, row_bytes);
    }

    copied_bytes += row_bytes * static_cast<size_t>(clipped.bottom - clipped.top);
}

void CopyFullFrame(std::span<uint8_t> capture_bgra,
                   const MappedFrameView& mapped,
                   uint64_t& copied_bytes) {
    if (capture_bgra.empty() || mapped.data == nullptr) {
        return;
    }

    const size_t total_bytes = static_cast<size_t>(mapped.row_pitch) * mapped.height;
    if (capture_bgra.size() < total_bytes) {
        return;
    }

    std::memcpy(capture_bgra.data(), mapped.data, total_bytes);
    copied_bytes += total_bytes;
}

void ReplayMoveRect(std::span<uint8_t> capture_bgra,
                    uint32_t capture_pitch,
                    uint32_t frame_width,
                    uint32_t frame_height,
                    const DXGI_OUTDUPL_MOVE_RECT& move_rect) {
    const CopyRegion destination = ClipRegion(
        CopyRegion{
            .left = move_rect.DestinationRect.left,
            .top = move_rect.DestinationRect.top,
            .right = move_rect.DestinationRect.right,
            .bottom = move_rect.DestinationRect.bottom,
        },
        frame_width,
        frame_height);
    if (!IsValidRegion(destination)) {
        return;
    }

    const LONG width = destination.right - destination.left;
    const LONG height = destination.bottom - destination.top;
    CopyRegion source{
        .left = move_rect.SourcePoint.x,
        .top = move_rect.SourcePoint.y,
        .right = move_rect.SourcePoint.x + width,
        .bottom = move_rect.SourcePoint.y + height,
    };
    source = ClipRegion(source, frame_width, frame_height);
    if (!IsValidRegion(source)) {
        return;
    }

    const LONG clipped_width = (std::min)(destination.right - destination.left,
                                          source.right - source.left);
    const LONG clipped_height = (std::min)(destination.bottom - destination.top,
                                           source.bottom - source.top);
    if (clipped_width <= 0 || clipped_height <= 0) {
        return;
    }

    const size_t row_bytes = static_cast<size_t>(clipped_width) * 4u;
    std::vector<uint8_t> temp(static_cast<size_t>(clipped_height) * row_bytes);
    auto* dst_base = capture_bgra.data();

    for (LONG row = 0; row < clipped_height; ++row) {
        const LONG src_y = source.top + row;
        const size_t src_offset = static_cast<size_t>(src_y) * capture_pitch +
                                  static_cast<size_t>(source.left) * 4u;
        std::memcpy(temp.data() + static_cast<size_t>(row) * row_bytes,
                    dst_base + src_offset,
                    row_bytes);
    }

    for (LONG row = 0; row < clipped_height; ++row) {
        const LONG dst_y = destination.top + row;
        const size_t dst_offset = static_cast<size_t>(dst_y) * capture_pitch +
                                  static_cast<size_t>(destination.left) * 4u;
        std::memcpy(dst_base + dst_offset,
                    temp.data() + static_cast<size_t>(row) * row_bytes,
                    row_bytes);
    }
}

} // namespace

FramePreparationResult PrepareCaptureFrame(const CaptureDamageDecision& decision,
                                          const MappedFrameView& mapped,
                                          const CapturedFrame& frame,
                                          std::span<uint8_t> capture_bgra,
                                          uint32_t capture_pitch) {
    FramePreparationResult result;
    if (capture_bgra.empty() || mapped.data == nullptr) {
        return result;
    }

    const auto copy_start = std::chrono::steady_clock::now();
    if (decision.copy_full_frame) {
        CopyFullFrame(capture_bgra, mapped, result.copied_bytes);
    } else {
        if (decision.replay_move_rects) {
            for (const DXGI_OUTDUPL_MOVE_RECT& move_rect : frame.move_rects) {
                ReplayMoveRect(capture_bgra, capture_pitch, mapped.width, mapped.height, move_rect);
            }
        }
        if (decision.copy_dirty_regions) {
            for (const DirtyRect& dirty_rect : frame.dirty_rects) {
                CopyRegionFromMapped(
                    capture_bgra,
                    capture_pitch,
                    mapped,
                    CopyRegion{
                        .left = dirty_rect.x,
                        .top = dirty_rect.y,
                        .right = dirty_rect.x + dirty_rect.width,
                        .bottom = dirty_rect.y + dirty_rect.height,
                    },
                    result.copied_bytes);
            }
        }
        if (decision.copy_previous_pointer_region && decision.previous_pointer_region) {
            CopyRegionFromMapped(
                capture_bgra,
                capture_pitch,
                mapped,
                *decision.previous_pointer_region,
                result.copied_bytes);
        }
        if (decision.copy_current_pointer_region && decision.current_pointer_region) {
            CopyRegionFromMapped(
                capture_bgra,
                capture_pitch,
                mapped,
                *decision.current_pointer_region,
                result.copied_bytes);
        }
    }
    result.copy_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - copy_start).count();

    if (decision.overlay_pointer) {
        const auto overlay_start = std::chrono::steady_clock::now();
        ScreenCapture::OverlayPointer(capture_bgra, capture_pitch, frame);
        result.overlay_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - overlay_start).count();
        result.pointer_overlay_applied = true;
    }

    return result;
}

} // namespace remotex::host
