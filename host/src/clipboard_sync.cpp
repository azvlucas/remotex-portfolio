// RemoteX — host/clipboard_sync.cpp

#include "host/clipboard_sync.h"

#include <shellapi.h>
#include <shlobj.h>
#include <cstring>

namespace remotex::host {

namespace {

static const wchar_t* kClassName = L"RemoteXClipboardMessageWindow";

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size_needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size_needed <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(size_needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), size_needed);
    return wide;
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || *value == L'\0') {
        return {};
    }

    const int size_needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 1) {
        return {};
    }

    std::string utf8(static_cast<size_t>(size_needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8.data(), size_needed, nullptr, nullptr);
    return utf8;
}

} // namespace

ClipboardSync::ClipboardSync() {}

ClipboardSync::~ClipboardSync() {
    Stop();
}

void ClipboardSync::Start() {
    if (running_) return;
    running_ = true;

    thread_ = std::thread(&ClipboardSync::MessageLoop, this);
}

void ClipboardSync::Stop() {
    if (!running_) return;
    running_ = false;

    if (hwnd_) {
        // Send a message to unblock GetMessage in thread
        PostMessageW(hwnd_, WM_QUIT, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void ClipboardSync::SetRemoteClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) {
        return;
    }

    EmptyClipboard();
    const bool wrote = WriteUnicodeTextToClipboard(Utf8ToWide(text));
    if (wrote) {
        is_updating_clipboard_ = true;
    }
    CloseClipboard();
}

void ClipboardSync::SetRemoteClipboardFiles(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) {
        return;
    }

    if (!OpenClipboard(nullptr)) {
        return;
    }

    EmptyClipboard();
    const bool wrote = WriteFileListToClipboard(paths);
    if (wrote) {
        is_updating_clipboard_ = true;
    }
    CloseClipboard();
}

bool ClipboardSync::WriteUnicodeTextToClipboard(const std::wstring& text) {
    HGLOBAL hgl = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!hgl) {
        return false;
    }

    auto* buffer = static_cast<wchar_t*>(GlobalLock(hgl));
    if (!buffer) {
        GlobalFree(hgl);
        return false;
    }

    if (!text.empty()) {
        std::memcpy(buffer, text.data(), text.size() * sizeof(wchar_t));
    }
    buffer[text.size()] = L'\0';
    GlobalUnlock(hgl);

    if (!SetClipboardData(CF_UNICODETEXT, hgl)) {
        GlobalFree(hgl);
        return false;
    }

    return true;
}

bool ClipboardSync::WriteFileListToClipboard(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::wstring> wide_paths;
    wide_paths.reserve(paths.size());
    size_t total_chars = 1; // final double-null terminator

    for (const auto& path : paths) {
        if (path.empty()) {
            continue;
        }

        std::wstring wide_path = path.wstring();
        if (wide_path.empty()) {
            continue;
        }

        total_chars += wide_path.size() + 1;
        wide_paths.push_back(std::move(wide_path));
    }

    if (wide_paths.empty()) {
        return false;
    }

    const size_t payload_bytes = sizeof(DROPFILES) + (total_chars * sizeof(wchar_t));
    HGLOBAL hdrop_memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, payload_bytes);
    if (!hdrop_memory) {
        return false;
    }

    auto* dropfiles = static_cast<DROPFILES*>(GlobalLock(hdrop_memory));
    if (!dropfiles) {
        GlobalFree(hdrop_memory);
        return false;
    }

    dropfiles->pFiles = sizeof(DROPFILES);
    dropfiles->fWide = TRUE;

    auto* destination = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(dropfiles) + sizeof(DROPFILES));
    for (const auto& wide_path : wide_paths) {
        std::memcpy(destination, wide_path.c_str(), wide_path.size() * sizeof(wchar_t));
        destination += wide_path.size();
        *destination++ = L'\0';
    }
    *destination = L'\0';

    GlobalUnlock(hdrop_memory);

    if (!SetClipboardData(CF_HDROP, hdrop_memory)) {
        GlobalFree(hdrop_memory);
        return false;
    }

    const UINT preferred_drop_effect = RegisterClipboardFormatA(CFSTR_PREFERREDDROPEFFECT);
    if (preferred_drop_effect != 0) {
        HGLOBAL effect_memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
        if (effect_memory) {
            auto* effect = static_cast<DWORD*>(GlobalLock(effect_memory));
            if (effect) {
                *effect = DROPEFFECT_COPY;
                GlobalUnlock(effect_memory);
                if (!SetClipboardData(preferred_drop_effect, effect_memory)) {
                    GlobalFree(effect_memory);
                }
            } else {
                GlobalFree(effect_memory);
            }
        }
    }

    return true;
}

void ClipboardSync::MessageLoop() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // Create a message-only window
    hwnd_ = CreateWindowExW(0, kClassName, L"ClipboardSyncWindow",
                            0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);

    if (hwnd_) {
        AddClipboardFormatListener(hwnd_);

        MSG msg;
        while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        RemoveClipboardFormatListener(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kClassName, wc.hInstance);
}

LRESULT CALLBACK ClipboardSync::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    ClipboardSync* self = nullptr;

    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* create = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<ClipboardSync*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ClipboardSync*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self && uMsg == WM_CLIPBOARDUPDATE) {
        if (self->is_updating_clipboard_) {
            // Ignore our very own clipboard injections
            self->is_updating_clipboard_ = false;
        } else {
            self->FetchAndInvokeCallback();
        }
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ClipboardSync::FetchAndInvokeCallback() {
    if (!on_clipboard_changed_) {
        return;
    }

    if (!OpenClipboard(hwnd_)) {
        return;
    }

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (auto hdrop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
            const UINT file_count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::filesystem::path> file_paths;
            file_paths.reserve(file_count);

            for (UINT index = 0; index < file_count; ++index) {
                const UINT path_length = DragQueryFileW(hdrop, index, nullptr, 0);
                if (path_length == 0) {
                    continue;
                }

                std::wstring buffer(static_cast<size_t>(path_length) + 1, L'\0');
                const UINT copied = DragQueryFileW(hdrop, index, buffer.data(), path_length + 1);
                if (copied == 0) {
                    continue;
                }

                buffer.resize(copied);
                file_paths.emplace_back(std::move(buffer));
            }

            if (!file_paths.empty()) {
                ClipboardPayload payload;
                payload.kind = ClipboardPayload::Kind::Files;
                payload.file_paths = std::move(file_paths);
                CloseClipboard();
                on_clipboard_changed_(payload);
                return;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (auto hgl = GetClipboardData(CF_UNICODETEXT)) {
            const wchar_t* buffer = static_cast<const wchar_t*>(GlobalLock(hgl));
            if (buffer) {
                ClipboardPayload payload;
                payload.kind = ClipboardPayload::Kind::Text;
                payload.text = WideToUtf8(buffer);
                GlobalUnlock(hgl);
                CloseClipboard();
                on_clipboard_changed_(payload);
                return;
            }
        }
    }

    CloseClipboard();
}

} // namespace remotex::host
