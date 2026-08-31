#pragma once

#include "host/capture_damage.h"

namespace remotex::host {

CopyRegion RotateRegion(const CopyRegion& src,
                        uint32_t source_width,
                        uint32_t source_height,
                        DXGI_MODE_ROTATION rotation);

DXGI_OUTDUPL_MOVE_RECT RotateMoveRect(const DXGI_OUTDUPL_MOVE_RECT& src,
                                      uint32_t source_width,
                                      uint32_t source_height,
                                      DXGI_MODE_ROTATION rotation);

POINT RotatePoint(POINT src,
                  uint32_t source_width,
                  uint32_t source_height,
                  DXGI_MODE_ROTATION rotation);

POINT RotatePointerDrawOrigin(POINT pointer_position,
                              POINT hotspot,
                              uint32_t draw_width,
                              uint32_t draw_height,
                              uint32_t source_width,
                              uint32_t source_height,
                              DXGI_MODE_ROTATION rotation);

POINT MapPointerDrawOriginIntoDesktopRegion(POINT desktop_pointer_position,
                                            POINT hotspot,
                                            const RECT& desktop_region);

} // namespace remotex::host
