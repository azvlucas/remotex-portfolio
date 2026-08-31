#pragma once

#include "host/screen_capture.h"

#include <optional>
#include <vector>

namespace remotex::host {

struct CopyRegion {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

enum class CaptureUpdateKind {
    FullRefresh,
    PartialUpdate,
    PointerPriorityUpdate,
};

struct PointerSnapshot {
    std::optional<CopyRegion> region;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info{};
    bool visible = false;
};

struct CaptureDamageContext {
    bool force_first_frame = false;
    bool structural_change = false;
    bool buffer_resized = false;
    const CapturedFrame* frame = nullptr;
    PointerSnapshot previous_pointer{};
};

struct CaptureDamageDecision {
    CaptureUpdateKind kind = CaptureUpdateKind::FullRefresh;
    std::optional<CopyRegion> previous_pointer_region;
    std::optional<CopyRegion> current_pointer_region;
    bool force_first_frame = false;
    bool structural_change = false;
    bool buffer_resized = false;
    bool missing_damage_metadata = false;
    bool move_rects_present = false;
    bool dirty_rects_present = false;
    bool pointer_changed = false;
    bool dirty_touches_previous_pointer = false;
    bool dirty_touches_current_pointer = false;
    bool copy_full_frame = false;
    bool replay_move_rects = false;
    bool copy_dirty_regions = false;
    bool copy_previous_pointer_region = false;
    bool copy_current_pointer_region = false;
    bool overlay_pointer = false;
    bool should_send = false;
};

bool IsValidRegion(const CopyRegion& region);
CopyRegion ClipRegion(const CopyRegion& region, uint32_t width, uint32_t height);
bool RegionsIntersect(const CopyRegion& lhs, const CopyRegion& rhs);
std::optional<CopyRegion> PointerRegion(const CapturedFrame& frame);
bool PointerShapeChanged(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& lhs,
                         const DXGI_OUTDUPL_POINTER_SHAPE_INFO& rhs);
bool PointerStateChanged(const CapturedFrame& frame,
                         const PointerSnapshot& previous_pointer,
                         const std::optional<CopyRegion>& current_pointer_region);
std::vector<CopyRegion> CollectCopiedRegions(const CaptureDamageDecision& decision,
                                             const CapturedFrame& frame);
CaptureDamageDecision ClassifyCaptureDamage(const CaptureDamageContext& context);

} // namespace remotex::host
