#pragma once
// RemoteX — host/clipboard_sync.h
// Monitora e injeta cópias da área de transferência no SO via Win32 API.

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <filesystem>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remotex::host {

class ClipboardSync {
public:
    struct ClipboardPayload {
        enum class Kind {
            Text,
            Files,
        };

        Kind kind = Kind::Text;
        std::string text;
        std::vector<std::filesystem::path> file_paths;

        [[nodiscard]] bool IsText() const {
            return kind == Kind::Text;
        }

        [[nodiscard]] bool IsFiles() const {
            return kind == Kind::Files;
        }
    };

    ClipboardSync();
    ~ClipboardSync();

    void Start();
    void Stop();

    // Callback invoked when local system clipboard changes
    using ClipboardChangedCallback = std::function<void(const ClipboardPayload&)>;
    void SetOnClipboardChanged(ClipboardChangedCallback callback) { on_clipboard_changed_ = std::move(callback); }

    // Overwrites the local system clipboard with the given string without triggering the callback
    void SetRemoteClipboard(const std::string& text);
    void SetRemoteClipboardFiles(const std::vector<std::filesystem::path>& paths);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void MessageLoop();
    void FetchAndInvokeCallback();
    bool WriteUnicodeTextToClipboard(const std::wstring& text);
    bool WriteFileListToClipboard(const std::vector<std::filesystem::path>& paths);

    std::thread              thread_;
    std::atomic<bool>        running_{false};
    HWND                     hwnd_{nullptr};

    std::atomic<bool>        is_updating_clipboard_{false};
    ClipboardChangedCallback on_clipboard_changed_;
};

} // namespace remotex::host
