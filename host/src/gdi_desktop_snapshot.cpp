// RemoteX - host/gdi_desktop_snapshot.cpp
// GDI fallback snapshot for secure-desktop timeout recovery.

#include "host/gdi_desktop_snapshot.h"

#include <cstring>

namespace remotex::host {

bool CaptureDesktopRegionGdi(const RECT& region,
                             GdiDesktopSnapshot& snapshot,
                             DWORD& error) {
    snapshot = {};
    error = ERROR_SUCCESS;

    const LONG width = region.right - region.left;
    const LONG height = region.bottom - region.top;
    if (width <= 0 || height <= 0) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        error = GetLastError();
        return false;
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (!memory_dc) {
        error = GetLastError();
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen_dc,
                                   &bitmap_info,
                                   DIB_RGB_COLORS,
                                   &bits,
                                   nullptr,
                                   0);
    if (!dib || !bits) {
        error = GetLastError();
        if (dib) {
            DeleteObject(dib);
        }
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HGDIOBJ previous_bitmap = SelectObject(memory_dc, dib);
    if (!previous_bitmap || previous_bitmap == HGDI_ERROR) {
        error = GetLastError();
        DeleteObject(dib);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    const BOOL copied = BitBlt(memory_dc,
                               0,
                               0,
                               width,
                               height,
                               screen_dc,
                               region.left,
                               region.top,
                               SRCCOPY | CAPTUREBLT);
    if (!copied) {
        error = GetLastError();
        SelectObject(memory_dc, previous_bitmap);
        DeleteObject(dib);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    snapshot.width = static_cast<uint32_t>(width);
    snapshot.height = static_cast<uint32_t>(height);
    snapshot.pitch = static_cast<uint32_t>(width) * 4u;
    snapshot.bgra.resize(static_cast<size_t>(snapshot.pitch) * snapshot.height);
    std::memcpy(snapshot.bgra.data(), bits, snapshot.bgra.size());

    SelectObject(memory_dc, previous_bitmap);
    DeleteObject(dib);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return true;
}

} // namespace remotex::host
