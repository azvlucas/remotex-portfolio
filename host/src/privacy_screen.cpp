// RemoteX — host/privacy_screen.cpp
// Fullscreen black overlay for privacy mode.

#include "host/privacy_screen.h"
#include <cstdio>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace remotex::host {

static const wchar_t kClassName[] = L"RemoteXPrivacyScreen";
static const wchar_t kDisplayText[] = L"RemoteX \u2014 Sess\u00e3o privada em andamento";

PrivacyScreen::~PrivacyScreen() {
    Disable();
}

void PrivacyScreen::Enable() {
    std::lock_guard lock(mutex_);
    if (enabled_.load()) return;

    enabled_.store(true);
    thread_ = std::thread(&PrivacyScreen::ThreadFunc, this);
}

void PrivacyScreen::Disable() {
    std::lock_guard lock(mutex_);
    if (!enabled_.load()) return;

    enabled_.store(false);

    // Signal the window thread to quit.
    if (thread_id_ != 0) {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    thread_id_ = 0;
    hwnd_ = nullptr;
}

void PrivacyScreen::ThreadFunc() {
    thread_id_ = GetCurrentThreadId();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.hbrBackground  = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName  = kClassName;
    wc.hCursor        = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);

    // Cover the entire virtual screen (all monitors).
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kClassName,
        L"RemoteX Privacy",
        WS_POPUP | WS_VISIBLE,
        x, y, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd_) {
        fprintf(stderr, "[PrivacyScreen] CreateWindowExW failed: %lu\n", GetLastError());
        enabled_.store(false);
        return;
    }

    // WS_EX_LAYERED requires setting attributes — make fully opaque visually.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    // Make the window invisible to screen capture but visible on the physical display.
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        fprintf(stderr, "[PrivacyScreen] SetWindowDisplayAffinity failed: %lu "
                "(WDA_EXCLUDEFROMCAPTURE requires Windows 10 2004+)\n", GetLastError());
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // Message loop — runs until WM_QUIT is posted from Disable().
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

LRESULT CALLBACK PrivacyScreen::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Fill black.
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        // Draw centered text.
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(200, 200, 200));

        HFONT font = CreateFontW(
            32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));

        DrawTextW(hdc, kDisplayText, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, old_font);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        // Prevent local user from closing.
        return 0;

    case WM_SYSCOMMAND:
        // Block Alt+F4 and other system commands that could dismiss the window.
        if ((wp & 0xFFF0) == SC_CLOSE || (wp & 0xFFF0) == SC_MINIMIZE)
            return 0;
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_DISPLAYCHANGE:
        // Monitor config changed — resize to cover all monitors.
        SetWindowPos(hwnd, HWND_TOPMOST,
                     GetSystemMetrics(SM_XVIRTUALSCREEN),
                     GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN),
                     GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace remotex::host
