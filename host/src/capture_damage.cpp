#include "host/capture_damage.h"

#include <algorithm>

namespace remotex::host {

namespace {

bool DirtyRectsTouchRegion(const std::optional<CopyRegion>& region,
                           const std::vector<DirtyRect>& dirty_rects) {
    if (!region) {
        return false;
    }
    for (const auto& dirty_rect : dirty_rects) {
        const CopyRegion dirty{
            .left = dirty_rect.x,
            .top = dirty_rect.y,
            .right = dirty_rect.x + dirty_rect.width,
            .bottom = dirty_rect.y + dirty_rect.height,
        };
        if (RegionsIntersect(*region, dirty)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool IsValidRegion(const CopyRegion& region) {
    return region.right > region.left && region.bottom > region.top;
}

CopyRegion ClipRegion(const CopyRegion& region, uint32_t width, uint32_t height) {
    CopyRegion clipped = region;
    const LONG max_width = static_cast<LONG>(width);
    const LONG max_height = static_cast<LONG>(height);
    clipped.left = std::clamp(clipped.left, 0L, max_width);
    clipped.top = std::clamp(clipped.top, 0L, max_height);
    clipped.right = std::clamp(clipped.right, 0L, max_width);
    clipped.bottom = std::clamp(clipped.bottom, 0L, max_height);
    return clipped;
}

bool RegionsIntersect(const CopyRegion& lhs, const CopyRegion& rhs) {
    return lhs.left < rhs.right && rhs.left < lhs.right &&
           lhs.top < rhs.bottom && rhs.top < lhs.bottom;
}

std::optional<CopyRegion> PointerRegion(const CapturedFrame& frame) {
    if (!frame.pointer_visible || frame.pointer_shape_buffer.empty()) {
        return std::nullopt;
    }

    const auto& info = frame.pointer_shape_info;
    if (info.Width == 0 || info.Height == 0) {
        return std::nullopt;
    }

    const uint32_t draw_height =
        info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? info.Height / 2 : info.Height;
    if (draw_height == 0) {
        return std::nullopt;
    }

    CopyRegion region{
        .left = frame.pointer_x,
        .top = frame.pointer_y,
        .right = frame.pointer_x + static_cast<LONG>(info.Width),
        .bottom = frame.pointer_y + static_cast<LONG>(draw_height),
    };
    region = ClipRegion(region, frame.width, frame.height);
    if (!IsValidRegion(region)) {
        return std::nullopt;
    }
    return region;
}

bool PointerShapeChanged(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& lhs,
                         const DXGI_OUTDUPL_POINTER_SHAPE_INFO& rhs) {
    return lhs.Type != rhs.Type ||
           lhs.Width != rhs.Width ||
           lhs.Height != rhs.Height ||
           lhs.Pitch != rhs.Pitch ||
           lhs.HotSpot.x != rhs.HotSpot.x ||
           lhs.HotSpot.y != rhs.HotSpot.y;
}

bool PointerStateChanged(const CapturedFrame& frame,
                         const PointerSnapshot& previous_pointer,
                         const std::optional<CopyRegion>& current_pointer_region) {
    if (static_cast<bool>(current_pointer_region) != static_cast<bool>(previous_pointer.region)) {
        return true;
    }
    if (current_pointer_region && previous_pointer.region &&
        (current_pointer_region->left != previous_pointer.region->left ||
         current_pointer_region->top != previous_pointer.region->top ||
         current_pointer_region->right != previous_pointer.region->right ||
         current_pointer_region->bottom != previous_pointer.region->bottom)) {
        return true;
    }
    if (frame.pointer_visible != previous_pointer.visible) {
        return true;
    }
    if (frame.pointer_visible &&
        PointerShapeChanged(frame.pointer_shape_info, previous_pointer.shape_info)) {
        return true;
    }
    return false;
}

std::vector<CopyRegion> CollectCopiedRegions(const CaptureDamageDecision& decision,
                                             const CapturedFrame& frame) {
    std::vector<CopyRegion> regions;
    if (frame.width == 0 || frame.height == 0) {
        return regions;
    }

    auto append_region = [&](const CopyRegion& region) {
        const CopyRegion clipped = ClipRegion(region, frame.width, frame.height);
        if (IsValidRegion(clipped)) {
            regions.push_back(clipped);
        }
    };

    if (decision.copy_full_frame) {
        append_region(CopyRegion{
            .left = 0,
            .top = 0,
            .right = static_cast<LONG>(frame.width),
            .bottom = static_cast<LONG>(frame.height),
        });
        return regions;
    }

    if (decision.copy_dirty_regions) {
        for (const auto& dirty_rect : frame.dirty_rects) {
            append_region(CopyRegion{
                .left = dirty_rect.x,
                .top = dirty_rect.y,
                .right = dirty_rect.x + dirty_rect.width,
                .bottom = dirty_rect.y + dirty_rect.height,
            });
        }
    }

    if (decision.copy_previous_pointer_region && decision.previous_pointer_region) {
        append_region(*decision.previous_pointer_region);
    }
    if (decision.copy_current_pointer_region && decision.current_pointer_region) {
        append_region(*decision.current_pointer_region);
    }

    return regions;
}

CaptureDamageDecision ClassifyCaptureDamage(const CaptureDamageContext& context) {
    CaptureDamageDecision decision;
    decision.force_first_frame = context.force_first_frame;
    decision.structural_change = context.structural_change;
    decision.buffer_resized = context.buffer_resized;

    if (context.frame == nullptr) {
        decision.copy_full_frame = true;
        decision.should_send = true;
        return decision;
    }

    const auto& frame = *context.frame;
    decision.previous_pointer_region = context.previous_pointer.region;
    decision.current_pointer_region = PointerRegion(frame);
    decision.move_rects_present = !frame.move_rects.empty();
    decision.dirty_rects_present = !frame.dirty_rects.empty();
    decision.pointer_changed =
        PointerStateChanged(frame, context.previous_pointer, decision.current_pointer_region);
    decision.dirty_touches_previous_pointer =
        DirtyRectsTouchRegion(decision.previous_pointer_region, frame.dirty_rects);
    decision.dirty_touches_current_pointer =
        DirtyRectsTouchRegion(decision.current_pointer_region, frame.dirty_rects);
    decision.missing_damage_metadata =
        !decision.dirty_rects_present &&
        !decision.move_rects_present &&
        !decision.pointer_changed;

    if (decision.force_first_frame ||
        decision.structural_change ||
        decision.buffer_resized ||
        decision.missing_damage_metadata) {
        decision.kind = CaptureUpdateKind::FullRefresh;
        decision.copy_full_frame = true;
        decision.overlay_pointer = decision.current_pointer_region.has_value();
        decision.should_send = true;
        return decision;
    }

    if (decision.pointer_changed &&
        !decision.dirty_rects_present &&
        !decision.move_rects_present) {
        decision.kind = CaptureUpdateKind::PointerPriorityUpdate;
        decision.copy_previous_pointer_region = decision.previous_pointer_region.has_value();
        decision.copy_current_pointer_region = decision.current_pointer_region.has_value();
        decision.overlay_pointer = decision.current_pointer_region.has_value();
        decision.should_send =
            decision.copy_previous_pointer_region ||
            decision.copy_current_pointer_region ||
            decision.overlay_pointer;
        return decision;
    }

    decision.kind = CaptureUpdateKind::PartialUpdate;
    decision.replay_move_rects = decision.move_rects_present;
    decision.copy_dirty_regions = decision.dirty_rects_present;
    decision.copy_previous_pointer_region =
        decision.previous_pointer_region.has_value() &&
        (decision.pointer_changed || decision.dirty_touches_previous_pointer);
    decision.copy_current_pointer_region =
        decision.current_pointer_region.has_value() &&
        (decision.pointer_changed || decision.dirty_touches_current_pointer);
    decision.overlay_pointer =
        decision.current_pointer_region.has_value() &&
        (decision.pointer_changed || decision.dirty_touches_current_pointer);
    decision.should_send =
        decision.replay_move_rects ||
        decision.copy_dirty_regions ||
        decision.copy_previous_pointer_region ||
        decision.copy_current_pointer_region ||
        decision.overlay_pointer;
    return decision;
}

} // namespace remotex::host
