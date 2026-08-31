#include "host/rotation_geometry.h"

namespace remotex::host {

CopyRegion RotateRegion(const CopyRegion& src,
                        uint32_t source_width,
                        uint32_t source_height,
                        DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
        return CopyRegion{
            .left = static_cast<LONG>(source_height) - src.bottom,
            .top = src.left,
            .right = static_cast<LONG>(source_height) - src.top,
            .bottom = src.right,
        };
    case DXGI_MODE_ROTATION_ROTATE180:
        return CopyRegion{
            .left = static_cast<LONG>(source_width) - src.right,
            .top = static_cast<LONG>(source_height) - src.bottom,
            .right = static_cast<LONG>(source_width) - src.left,
            .bottom = static_cast<LONG>(source_height) - src.top,
        };
    case DXGI_MODE_ROTATION_ROTATE270:
        return CopyRegion{
            .left = src.top,
            .top = static_cast<LONG>(source_width) - src.right,
            .right = src.bottom,
            .bottom = static_cast<LONG>(source_width) - src.left,
        };
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    default:
        return src;
    }
}

DXGI_OUTDUPL_MOVE_RECT RotateMoveRect(const DXGI_OUTDUPL_MOVE_RECT& src,
                                      uint32_t source_width,
                                      uint32_t source_height,
                                      DXGI_MODE_ROTATION rotation) {
    const CopyRegion rotated_destination = RotateRegion(
        CopyRegion{
            .left = src.DestinationRect.left,
            .top = src.DestinationRect.top,
            .right = src.DestinationRect.right,
            .bottom = src.DestinationRect.bottom,
        },
        source_width,
        source_height,
        rotation);

    const POINT rotated_source = RotatePoint(src.SourcePoint, source_width, source_height, rotation);

    return DXGI_OUTDUPL_MOVE_RECT{
        .SourcePoint = rotated_source,
        .DestinationRect = RECT{
            rotated_destination.left,
            rotated_destination.top,
            rotated_destination.right,
            rotated_destination.bottom,
        },
    };
}

POINT RotatePoint(POINT src,
                  uint32_t source_width,
                  uint32_t source_height,
                  DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
        return POINT{
            static_cast<LONG>(source_height) - src.y - 1,
            src.x,
        };
    case DXGI_MODE_ROTATION_ROTATE180:
        return POINT{
            static_cast<LONG>(source_width) - src.x - 1,
            static_cast<LONG>(source_height) - src.y - 1,
        };
    case DXGI_MODE_ROTATION_ROTATE270:
        return POINT{
            src.y,
            static_cast<LONG>(source_width) - src.x - 1,
        };
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    default:
        return src;
    }
}

POINT RotatePointerDrawOrigin(POINT pointer_position,
                              POINT hotspot,
                              uint32_t draw_width,
                              uint32_t draw_height,
                              uint32_t source_width,
                              uint32_t source_height,
                              DXGI_MODE_ROTATION rotation) {
    const CopyRegion rotated_draw_region = RotateRegion(
        CopyRegion{
            .left = pointer_position.x - hotspot.x,
            .top = pointer_position.y - hotspot.y,
            .right = pointer_position.x - hotspot.x + static_cast<LONG>(draw_width),
            .bottom = pointer_position.y - hotspot.y + static_cast<LONG>(draw_height),
        },
        source_width,
        source_height,
        rotation);

    return POINT{
        rotated_draw_region.left,
        rotated_draw_region.top,
    };
}

POINT MapPointerDrawOriginIntoDesktopRegion(POINT desktop_pointer_position,
                                            POINT hotspot,
                                            const RECT& desktop_region) {
    return POINT{
        desktop_pointer_position.x - hotspot.x - desktop_region.left,
        desktop_pointer_position.y - hotspot.y - desktop_region.top,
    };
}

} // namespace remotex::host
