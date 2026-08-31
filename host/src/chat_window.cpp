// RemoteX — host/chat_window.cpp

#include "host/chat_window.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>

namespace remotex::host {

static const wchar_t* kChatClassName = L"RemoteXChatWindow";
static constexpr int kWindowWidth  = 400;
static constexpr int kWindowHeight = 320;
static constexpr int kInputHeight  = 28;
static constexpr int kButtonWidth  = 70;
static constexpr int kMargin       = 6;

// Control IDs.
static constexpr int IDC_HISTORY = 1001;
static constexpr int IDC_INPUT   = 1002;
static constexpr int IDC_SEND    = 1003;

ChatWindow::ChatWindow() {
    InitializeCriticalSection(&msg_cs_);
}

ChatWindow::~ChatWindow() {
    Stop();
    DeleteCriticalSection(&msg_cs_);
}

void ChatWindow::Start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&ChatWindow::MessageLoop, this);
}

void ChatWindow::Stop() {
    if (!running_) return;
    running_ = false;

    if (hwnd_) {
        PostMessageW(hwnd_, WM_CHAT_CLOSE, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void ChatWindow::ShowAndAddMessage(const std::string& sender, const std::string& text) {
    EnterCriticalSection(&msg_cs_);
    pending_messages_.push_back({sender, text});
    LeaveCriticalSection(&msg_cs_);

    if (hwnd_) {
        PostMessageW(hwnd_, WM_CHAT_MESSAGE, 0, 0);
    }
}

void ChatWindow::MessageLoop() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.lpszClassName  = kChatClassName;
    wc.hCursor        = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    CreateChatWindow();

    if (hwnd_) {
        MSG msg;
        while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_QUIT) break;
            // Intercept Enter key in the input field.
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
                msg.hwnd == hwnd_input_) {
                OnSendClicked();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }

    UnregisterClassW(kChatClassName, GetModuleHandleW(nullptr));
}

void ChatWindow::CreateChatWindow() {
    // Position at bottom-right of primary monitor.
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = screen_w - kWindowWidth - 20;
    int y = screen_h - kWindowHeight - 60;

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST,
        kChatClassName,
        L"RemoteX - Chat com Suporte",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        x, y, kWindowWidth, kWindowHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) return;

    font_ = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    int history_h = ch - kInputHeight - kMargin * 3;

    // History (multiline, read-only EDIT).
    hwnd_history_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        kMargin, kMargin, cw - kMargin * 2, history_h,
        hwnd_, (HMENU)(INT_PTR)IDC_HISTORY, GetModuleHandleW(nullptr), nullptr);

    // Input field.
    hwnd_input_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        kMargin, kMargin + history_h + kMargin,
        cw - kMargin * 3 - kButtonWidth, kInputHeight,
        hwnd_, (HMENU)(INT_PTR)IDC_INPUT, GetModuleHandleW(nullptr), nullptr);

    // Send button.
    hwnd_send_ = CreateWindowExW(
        0,
        L"BUTTON", L"Enviar",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cw - kMargin - kButtonWidth, kMargin + history_h + kMargin,
        kButtonWidth, kInputHeight,
        hwnd_, (HMENU)(INT_PTR)IDC_SEND, GetModuleHandleW(nullptr), nullptr);

    if (font_) {
        SendMessageW(hwnd_history_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessageW(hwnd_input_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessageW(hwnd_send_, WM_SETFONT, (WPARAM)font_, TRUE);
    }

    // Don't show yet — will be shown on first message.
}

void ChatWindow::AppendToHistory(const std::string& sender, const std::string& text) {
    if (!hwnd_history_) return;

    // Get current time.
    auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
    localtime_s(&tm_buf, &now_t);
    char time_str[16];
    std::snprintf(time_str, sizeof(time_str), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);

    const char* label = (sender == "viewer") ? "Suporte" : "Voce";

    std::ostringstream oss;
    oss << "[" << time_str << "] " << label << ": " << text << "\r\n";
    std::string line = oss.str();

    // Convert to wide string.
    int len = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0);
    std::wstring wline(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), wline.data(), len);

    // Append at end.
    int text_len = GetWindowTextLengthW(hwnd_history_);
    SendMessageW(hwnd_history_, EM_SETSEL, text_len, text_len);
    SendMessageW(hwnd_history_, EM_REPLACESEL, FALSE, (LPARAM)wline.c_str());

    // Scroll to bottom.
    SendMessageW(hwnd_history_, EM_SCROLLCARET, 0, 0);
}

void ChatWindow::OnSendClicked() {
    if (!hwnd_input_) return;

    int len = GetWindowTextLengthW(hwnd_input_);
    if (len <= 0) return;

    std::wstring wtext(len + 1, 0);
    GetWindowTextW(hwnd_input_, wtext.data(), len + 1);
    wtext.resize(len);

    // Convert to UTF-8.
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), (int)wtext.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(utf8_len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), (int)wtext.size(), utf8.data(), utf8_len, nullptr, nullptr);

    SetWindowTextW(hwnd_input_, L"");

    // Show in local history.
    AppendToHistory("host", utf8);

    // Invoke callback.
    if (send_cb_) {
        send_cb_(utf8);
    }
}

LRESULT CALLBACK ChatWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ChatWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<ChatWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ChatWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SEND && HIWORD(wParam) == BN_CLICKED) {
            self->OnSendClicked();
            return 0;
        }
        break;

    case WM_CHAT_MESSAGE: {
        // Drain pending messages.
        std::vector<PendingMessage> messages;
        EnterCriticalSection(&self->msg_cs_);
        messages.swap(self->pending_messages_);
        LeaveCriticalSection(&self->msg_cs_);

        for (const auto& m : messages) {
            self->AppendToHistory(m.sender, m.text);
        }

        // Show window on first message.
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return 0;
    }

    case WM_CHAT_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        // Just hide instead of destroying — session may still be active.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_SIZE: {
        int cw = LOWORD(lParam);
        int ch = HIWORD(lParam);
        int history_h = ch - kInputHeight - kMargin * 3;
        if (self->hwnd_history_) {
            MoveWindow(self->hwnd_history_, kMargin, kMargin, cw - kMargin * 2, history_h, TRUE);
        }
        if (self->hwnd_input_) {
            MoveWindow(self->hwnd_input_, kMargin, kMargin + history_h + kMargin,
                       cw - kMargin * 3 - kButtonWidth, kInputHeight, TRUE);
        }
        if (self->hwnd_send_) {
            MoveWindow(self->hwnd_send_, cw - kMargin - kButtonWidth,
                       kMargin + history_h + kMargin, kButtonWidth, kInputHeight, TRUE);
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace remotex::host
