// RemoteX - host/input_injector.cpp
// Inject remote input into Windows via SendInput.

#include "host/input_injector.h"
#include "host/desktop_context.h"

#include <algorithm>

namespace remotex::host {

using namespace remotex::core;

namespace {

constexpr SHORT kToggleBitMask = 0x0001;

bool IsSecureDesktopContext(const DesktopContextSnapshot& snapshot)
{
    return snapshot.input_desktop_available &&
           !snapshot.input_desktop_name.empty() &&
           _stricmp(snapshot.input_desktop_name.c_str(), "default") != 0;
}

} // namespace

void InputInjector::Start()
{
    UpdateDesktopMetrics();
    running_ = true;
}

void InputInjector::SetCapturedMonitorBounds(LONG left,
                                             LONG top,
                                             LONG right,
                                             LONG bottom)
{
    UpdateDesktopMetrics();
    monitor_left_   = left;
    monitor_top_    = top;
    monitor_width_  = right - left;
    monitor_height_ = bottom - top;
    if (monitor_width_  <= 0) monitor_width_  = 1920;
    if (monitor_height_ <= 0) monitor_height_ = 1080;
}

void InputInjector::Stop()
{
    running_ = false;
}

bool InputInjector::SubmitInputEvent(INPUT& input, DWORD& error)
{
    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

InputInjector::ProcessInputResult InputInjector::ProcessInputData(std::span<const uint8_t> data)
{
    ProcessInputResult result;
    if (!running_) {
        return result;
    }

    const DesktopContextSnapshot desktop_before = desktop_context_provider_
        ? desktop_context_provider_()
        : CaptureDesktopContext();
    result.input_desktop_name = desktop_before.input_desktop_name;
    result.thread_desktop_name = desktop_before.thread_desktop_name;

    UpdateDesktopMetrics();

    if (IsSecureDesktopContext(desktop_before) && secure_desktop_bridge_ != nullptr) {
        if (secure_desktop_bridge_->Submit(
                data,
                monitor_left_,
                monitor_top_,
                monitor_left_ + monitor_width_,
                monitor_top_ + monitor_height_,
                result)) {
            result.secure_desktop_bridge_attempted = true;
            result.secure_desktop_bridge_used = true;
            if (result.input_desktop_name.empty()) {
                result.input_desktop_name = desktop_before.input_desktop_name;
            }
            if (result.thread_desktop_name.empty()) {
                result.thread_desktop_name = desktop_before.thread_desktop_name;
            }
            return result;
        }
        result.secure_desktop_bridge_attempted = true;
    }

    result.desktop_switch_attempted =
        desktop_before.HasCompleteContext() && !desktop_before.InputMatchesThread();

    DWORD switch_error = ERROR_SUCCESS;
    DesktopContextSnapshot desktop_snapshot;
    const bool switched = SwitchCurrentThreadToInputDesktop(switch_error, &desktop_snapshot);
    result.desktop_switch_succeeded = switched || !result.desktop_switch_attempted;
    result.desktop_switch_error = switch_error;
    if (!desktop_snapshot.input_desktop_name.empty()) {
        result.input_desktop_name = desktop_snapshot.input_desktop_name;
    }
    if (!desktop_snapshot.thread_desktop_name.empty()) {
        result.thread_desktop_name = desktop_snapshot.thread_desktop_name;
    }

    size_t offset = 0;
    while (offset < data.size()) {
        auto remaining = data.subspan(offset);
        auto decoded = Deserialize(remaining);
        if (!decoded) {
            result.malformed_input = true;
            break;
        }

        const auto& event = decoded->event;
        DWORD event_error = ERROR_SUCCESS;
        bool injected = true;

        switch (event.type) {
            case InputEventType::MouseMove:
                injected = InjectMouseMove(event.mouse_move, event_error);
                break;
            case InputEventType::MouseButtonDown:
            case InputEventType::MouseButtonUp:
                injected = InjectMouseButton(event.mouse_button, event_error);
                break;
            case InputEventType::MouseScroll:
                injected = InjectMouseScroll(event.mouse_scroll, event_error);
                break;
            case InputEventType::KeyDown:
            case InputEventType::KeyUp:
                injected = InjectKey(event.key, event_error);
                break;
        }

        result.events_processed++;
        if (!injected) {
            result.send_input_failures++;
            result.last_send_input_error = event_error;
        }

        offset += decoded->bytes_consumed;
    }

    return result;
}

bool InputInjector::InjectMouseMove(const MouseMoveEvent& ev, DWORD& error)
{
    LONG abs_x = 0;
    LONG abs_y = 0;
    NormalizedToAbsolute(ev.x, ev.y, abs_x, abs_y);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = abs_x;
    input.mi.dy = abs_y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    return SubmitInputEvent(input, error);
}

bool InputInjector::InjectMouseButton(const MouseButtonEvent& ev, DWORD& error)
{
    INPUT input{};
    input.type = INPUT_MOUSE;

    const bool down = (ev.type == InputEventType::MouseButtonDown);

    switch (ev.button) {
        case MouseButton::Left:
            input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case MouseButton::Right:
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButton::Middle:
            input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
    }

    return SubmitInputEvent(input, error);
}

bool InputInjector::InjectMouseScroll(const MouseScrollEvent& ev, DWORD& error)
{
    bool ok = true;
    if (ev.delta_y != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(ev.delta_y * WHEEL_DELTA);
        ok = SubmitInputEvent(input, error) && ok;
    }

    if (ev.delta_x != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(ev.delta_x * WHEEL_DELTA);
        ok = SubmitInputEvent(input, error) && ok;
    }

    return ok;
}

bool InputInjector::IsExtendedKey(uint16_t vkey_code)
{
    switch (vkey_code) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
        case VK_NUMLOCK:
            return true;
        default:
            return false;
    }
}

InputInjector::NumLockAlignmentAction InputInjector::DetermineNumLockAlignmentAction(
    std::optional<bool> current_state,
    bool requested_state)
{
    if (!current_state.has_value()) {
        return NumLockAlignmentAction::CannotDetermine;
    }

    return current_state.value() == requested_state
        ? NumLockAlignmentAction::NoOp
        : NumLockAlignmentAction::ToggleOnce;
}

InputInjector::NumLockAlignmentResult InputInjector::AlignNumLockToPolicy(bool requested_state)
{
    NumLockAlignmentResult result{};
    result.requested_state = requested_state;
    result.current_state = ReadNumLockState();
    result.action = DetermineNumLockAlignmentAction(result.current_state, requested_state);

    if (result.action == NumLockAlignmentAction::CannotDetermine) {
        result.windows_error = ERROR_INVALID_STATE;
        return result;
    }

    if (result.action == NumLockAlignmentAction::NoOp) {
        result.applied = true;
        return result;
    }

    const KeyEvent key_down{InputEventType::KeyDown, VK_NUMLOCK};
    const KeyEvent key_up{InputEventType::KeyUp, VK_NUMLOCK};
    DWORD ignored_error = ERROR_SUCCESS;
    InjectKey(key_down, ignored_error);
    InjectKey(key_up, ignored_error);
    result.applied = true;
    return result;
}

bool InputInjector::InjectKey(const KeyEvent& ev, DWORD& error)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(ev.vkey_code);
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(ev.vkey_code, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = KEYEVENTF_SCANCODE;

    if (ev.type == InputEventType::KeyUp) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    if (IsExtendedKey(ev.vkey_code)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    return SubmitInputEvent(input, error);
}

void InputInjector::UpdateDesktopMetrics()
{
    vdesk_x_      = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vdesk_y_      = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vdesk_width_  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vdesk_height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vdesk_width_ <= 0) {
        vdesk_width_ = 1920;
    }
    if (vdesk_height_ <= 0) {
        vdesk_height_ = 1080;
    }
}

LONG InputInjector::MapNormalizedCoordinateToAbsolute(float normalized,
                                                      LONG region_origin,
                                                      LONG region_size,
                                                      int desktop_origin,
                                                      int desktop_size)
{
    normalized = std::clamp(normalized, 0.0f, 1.0f);

    const float region_span = static_cast<float>(std::max<LONG>(region_size - 1, 0));
    const float desktop_span = static_cast<float>(std::max(desktop_size - 1, 1));
    const float pixel = static_cast<float>(region_origin) + (normalized * region_span);

    return static_cast<LONG>(std::clamp(
        ((pixel - static_cast<float>(desktop_origin)) / desktop_span) * 65535.0f + 0.5f,
        0.0f,
        65535.0f));
}

void InputInjector::NormalizedToAbsolute(float nx, float ny,
                                         LONG& abs_x, LONG& abs_y) const
{
    abs_x = MapNormalizedCoordinateToAbsolute(
        nx, monitor_left_, monitor_width_, vdesk_x_, vdesk_width_);
    abs_y = MapNormalizedCoordinateToAbsolute(
        ny, monitor_top_, monitor_height_, vdesk_y_, vdesk_height_);
}

std::optional<bool> InputInjector::ReadNumLockState() const
{
    const SHORT raw_state = GetKeyState(VK_NUMLOCK);
    return (raw_state & kToggleBitMask) != 0;
}

} // namespace remotex::host
