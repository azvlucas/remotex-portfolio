#pragma once
// RemoteX - host/gdi_desktop_snapshot.h
// Bounded GDI snapshot helper for secure-desktop fallback capture.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <vector>

namespace remotex::host {

struct GdiDesktopSnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    std::vector<uint8_t> bgra;
};

bool CaptureDesktopRegionGdi(const RECT& region,
                             GdiDesktopSnapshot& snapshot,
                             DWORD& error);

} // namespace remotex::host
