#pragma once
// RemoteX — host/input_injector.h
// Recebe input remoto via Data Channel, deserializa e injeta no SO via SendInput.

#include "core/input_protocol.h"
#include "core/webrtc_peer.h"
#include "host/desktop_context.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace remotex::host {

struct InputInjectorTestAccess;

class InputInjector {
public:
    struct ProcessInputResult {
        size_t events_processed = 0;
        bool desktop_switch_attempted = false;
        bool desktop_switch_succeeded = true;
        bool malformed_input = false;
        DWORD desktop_switch_error = ERROR_SUCCESS;
        uint32_t send_input_failures = 0;
        DWORD last_send_input_error = ERROR_SUCCESS;
        bool secure_desktop_bridge_attempted = false;
        bool secure_desktop_bridge_used = false;
        DWORD secure_desktop_bridge_error = ERROR_SUCCESS;
        std::string input_desktop_name;
        std::string thread_desktop_name;
    };

    class SecureDesktopBridge {
    public:
        virtual ~SecureDesktopBridge() = default;
        virtual bool Submit(std::span<const uint8_t> data,
                            LONG monitor_left,
                            LONG monitor_top,
                            LONG monitor_right,
                            LONG monitor_bottom,
                            ProcessInputResult& result) = 0;
    };

    enum class NumLockAlignmentAction {
        NoOp,
        ToggleOnce,
        CannotDetermine,
    };

    struct NumLockAlignmentResult {
        NumLockAlignmentAction action = NumLockAlignmentAction::CannotDetermine;
        bool applied = false;
        std::optional<bool> current_state;
        bool requested_state = false;
        DWORD windows_error = 0;

        [[nodiscard]] bool already_aligned() const
        {
            return action == NumLockAlignmentAction::NoOp &&
                   current_state.has_value() &&
                   current_state.value() == requested_state;
        }

        [[nodiscard]] bool needs_toggle() const
        {
            return action == NumLockAlignmentAction::ToggleOnce;
        }
    };

    InputInjector() = default;
    ~InputInjector() = default;

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    void Start();
    void Stop();

    bool IsRunning() const { return running_; }

    void SetCapturedMonitorBounds(LONG left,
                                  LONG top,
                                  LONG right,
                                  LONG bottom);

    ProcessInputResult ProcessInputData(std::span<const uint8_t> data);
    void SetSecureDesktopBridge(SecureDesktopBridge* bridge) { secure_desktop_bridge_ = bridge; }
    NumLockAlignmentResult AlignNumLockToPolicy(bool requested_state);

    static LONG MapNormalizedCoordinateToAbsolute(float normalized,
                                                  LONG region_origin,
                                                  LONG region_size,
                                                  int desktop_origin,
                                                  int desktop_size);
    static bool IsExtendedKey(uint16_t vkey_code);
    static NumLockAlignmentAction DetermineNumLockAlignmentAction(
        std::optional<bool> current_state,
        bool requested_state);

private:
    friend struct InputInjectorTestAccess;

    using DesktopContextProvider = DesktopContextSnapshot (*)();

    static bool SubmitInputEvent(INPUT& input, DWORD& error);

    bool InjectMouseMove(const remotex::core::MouseMoveEvent& ev, DWORD& error);
    bool InjectMouseButton(const remotex::core::MouseButtonEvent& ev, DWORD& error);
    bool InjectMouseScroll(const remotex::core::MouseScrollEvent& ev, DWORD& error);
    bool InjectKey(const remotex::core::KeyEvent& ev, DWORD& error);

    void UpdateDesktopMetrics();
    void NormalizedToAbsolute(float nx, float ny, LONG& abs_x, LONG& abs_y) const;
    std::optional<bool> ReadNumLockState() const;

    std::atomic<bool> running_ = false;
    DesktopContextProvider desktop_context_provider_ = &CaptureDesktopContext;
    SecureDesktopBridge* secure_desktop_bridge_ = nullptr;

    int vdesk_x_ = 0;
    int vdesk_y_ = 0;
    int vdesk_width_ = 1920;
    int vdesk_height_ = 1080;

    LONG monitor_left_ = 0;
    LONG monitor_top_ = 0;
    LONG monitor_width_ = 1920;
    LONG monitor_height_ = 1080;
};

struct InputInjectorTestAccess {
    static void SetVirtualDesktopBounds(InputInjector& injector,
                                        int desktop_x,
                                        int desktop_y,
                                        int desktop_width,
                                        int desktop_height)
    {
        injector.vdesk_x_ = desktop_x;
        injector.vdesk_y_ = desktop_y;
        injector.vdesk_width_ = desktop_width > 0 ? desktop_width : 1920;
        injector.vdesk_height_ = desktop_height > 0 ? desktop_height : 1080;
    }

    static void NormalizedToAbsolute(const InputInjector& injector,
                                     float normalized_x,
                                     float normalized_y,
                                     LONG& abs_x,
                                     LONG& abs_y)
    {
        injector.NormalizedToAbsolute(normalized_x, normalized_y, abs_x, abs_y);
    }

    static void SetDesktopContextProvider(InputInjector& injector,
                                          InputInjector::DesktopContextProvider provider)
    {
        injector.desktop_context_provider_ = provider ? provider : &CaptureDesktopContext;
    }
};

} // namespace remotex::host
