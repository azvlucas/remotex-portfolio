#pragma once
// RemoteX — host/privacy_screen.h
// Fullscreen black overlay that hides the physical display during remote sessions.
// Uses WDA_EXCLUDEFROMCAPTURE so DXGI Desktop Duplication still sees the real desktop.

#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace remotex::host {

class PrivacyScreen {
public:
    PrivacyScreen() = default;
    ~PrivacyScreen();

    PrivacyScreen(const PrivacyScreen&) = delete;
    PrivacyScreen& operator=(const PrivacyScreen&) = delete;

    void Enable();
    void Disable();
    bool IsEnabled() const { return enabled_.load(); }

private:
    void ThreadFunc();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    std::atomic<bool> enabled_{false};
    std::mutex mutex_;
    std::thread thread_;
    HWND hwnd_ = nullptr;
    DWORD thread_id_ = 0;
};

} // namespace remotex::host
