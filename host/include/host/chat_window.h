#pragma once
// RemoteX — host/chat_window.h
// Janela Win32 de chat exibida no desktop do usuário durante sessão remota.

#include <string>
#include <functional>
#include <thread>
#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remotex::host {

class ChatWindow {
public:
    ChatWindow();
    ~ChatWindow();

    // Shows the window (lazy-created) and appends a message to history.
    void ShowAndAddMessage(const std::string& sender, const std::string& text);

    // Callback invoked when the local user sends a message.
    using SendCallback = std::function<void(const std::string& text)>;
    void SetSendCallback(SendCallback cb) { send_cb_ = std::move(cb); }

    void Start();
    void Stop();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void MessageLoop();
    void CreateChatWindow();
    void AppendToHistory(const std::string& sender, const std::string& text);
    void OnSendClicked();

    static constexpr UINT WM_CHAT_MESSAGE = WM_APP + 100;
    static constexpr UINT WM_CHAT_CLOSE   = WM_APP + 101;

    struct PendingMessage {
        std::string sender;
        std::string text;
    };

    std::thread        thread_;
    std::atomic<bool>  running_{false};

    HWND hwnd_         = nullptr;
    HWND hwnd_history_ = nullptr;
    HWND hwnd_input_   = nullptr;
    HWND hwnd_send_    = nullptr;
    HFONT font_        = nullptr;

    SendCallback send_cb_;

    // Simple thread-safe queue for pending messages (protected by critsec).
    CRITICAL_SECTION   msg_cs_;
    std::vector<PendingMessage> pending_messages_;
};

} // namespace remotex::host
