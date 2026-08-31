// RemoteX — viewer/input_capture.cpp
// Captura input Qt e envia via WebRTC Data Channel.
// Inclui WH_KEYBOARD_LL hook para interceptar teclas de sistema (Win, Alt+Tab).

#include "viewer/input_capture.h"

#include "viewer/video_renderer.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>
#include <QApplication>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace remotex::viewer {

using namespace remotex::core;

// ─── Label do Data Channel de input ─────────────────────────────────────────
static constexpr const char* kInputChannel = "input";

// ─── Instância ativa do hook (apenas uma por processo) ──────────────────────
InputCapture* InputCapture::s_active_instance_ = nullptr;

// ─── Construção ─────────────────────────────────────────────────────────────

InputCapture::InputCapture(WebRTCPeer* webrtc_peer,
                           QWidget* target,
                           QObject* parent)
    : QObject(parent)
    , peer_(webrtc_peer)
    , target_(target)
{
    if (target_) {
        target_->installEventFilter(this);
        target_->setMouseTracking(true);
        target_->setFocusPolicy(Qt::StrongFocus);
    }
}

InputCapture::~InputCapture()
{
    RemoveKeyboardHook();
    if (target_)
        target_->removeEventFilter(this);
}

void InputCapture::SetEnabled(bool enabled) {
    enabled_ = enabled;
    UpdateHookState();
}

void InputCapture::SetRemoteResolution(int width, int height) {
    remote_w_ = (width > 0)  ? width  : 1920;
    remote_h_ = (height > 0) ? height : 1080;
}

std::optional<bool> InputCapture::ReadLocalNumLockState()
{
    const SHORT raw_state = GetKeyState(VK_NUMLOCK);
    return (raw_state & 0x0001) != 0;
}

// ─── Low-level keyboard hook ────────────────────────────────────────────────

bool InputCapture::IsSystemKey(DWORD vk_code)
{
    switch (vk_code) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_TAB:      // Alt+Tab (hook verifica Alt held)
        case VK_ESCAPE:   // Ctrl+Esc / Alt+Esc
        case VK_SPACE:    // Alt+Space (system menu)
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK InputCapture::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_active_instance_) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool key_up    = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        bool is_extended = (kb->flags & LLKHF_EXTENDED) != 0;
        bool injected  = (kb->flags & LLKHF_INJECTED) != 0;

        // Não interceptar eventos que nós mesmos injetamos (evitar loop).
        if (!injected && s_active_instance_->HandleHookedKey(kb->vkCode, key_up, is_extended))
            return 1; // Consumir — não passar para o Windows.
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool InputCapture::HandleHookedKey(DWORD vk_code, bool key_up, bool is_extended)
{
    if (!enabled_ || !peer_)
        return false;

    // Determinar se esta tecla deve ser interceptada.
    bool should_intercept = false;

    // Win key sempre interceptar.
    if (vk_code == VK_LWIN || vk_code == VK_RWIN) {
        should_intercept = true;
    }
    // Alt+Tab → interceptar Tab quando Alt está pressionada.
    else if (vk_code == VK_TAB && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        should_intercept = true;
    }
    // Alt+Esc → interceptar Esc quando Alt está pressionada.
    else if (vk_code == VK_ESCAPE && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        should_intercept = true;
    }
    // Ctrl+Esc (equivalente a Win) → interceptar Esc quando Ctrl está pressionada.
    else if (vk_code == VK_ESCAPE && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        should_intercept = true;
    }
    // Alt+Space (menu da janela) → interceptar Space quando Alt está pressionada.
    else if (vk_code == VK_SPACE && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        should_intercept = true;
    }
    // Qualquer tecla com Win pressionada (Win+E, Win+D, Win+R, etc.)
    else if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
        should_intercept = true;
    }

    if (!should_intercept)
        return false;

    // Construir e enviar o KeyEvent para a máquina remota.
    KeyEvent ev;
    ev.type = key_up ? InputEventType::KeyUp : InputEventType::KeyDown;
    ev.vkey_code = static_cast<uint16_t>(vk_code);

    // Montar modifiers a partir do estado real das teclas.
    uint16_t mods = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= ModShift;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)  mods |= ModCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)     mods |= ModAlt;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        mods |= ModWin;
    ev.modifiers = mods;

    std::vector<uint8_t> buf;
    SerializeKey(ev, buf);
    SendInputData(buf);
    emit EventSent(ev.type);

    return true;
}

void InputCapture::InstallKeyboardHook()
{
    if (keyboard_hook_)
        return; // Já instalado.

    s_active_instance_ = this;
    keyboard_hook_ = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0  // 0 = todos os threads (obrigatório para LL hooks)
    );
}

void InputCapture::RemoveKeyboardHook()
{
    if (keyboard_hook_) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    if (s_active_instance_ == this)
        s_active_instance_ = nullptr;
}

void InputCapture::UpdateHookState()
{
    if (!target_)
        return;

    // Instalar hook apenas se habilitado E a janela da sessão tem foco.
    bool want_hook = enabled_ && target_->window()->isActiveWindow();

    if (want_hook && !keyboard_hook_)
        InstallKeyboardHook();
    else if (!want_hook && keyboard_hook_)
        RemoveKeyboardHook();
}

// ─── Event filter ───────────────────────────────────────────────────────────

bool InputCapture::eventFilter(QObject* watched, QEvent* event)
{
    if (!enabled_ || watched != target_)
        return QObject::eventFilter(watched, event);

    // Atualizar estado do hook quando a janela ganha/perde foco.
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
        event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate) {
        UpdateHookState();
    }

    std::vector<uint8_t> buf;

    switch (event->type()) {

    // ── Mouse Move ──────────────────────────────────────────────────────
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        MouseMoveEvent ev;
        WidgetPosToNormalized(me->position(), ev.x, ev.y);
        SerializeMouseMove(ev, buf);
        SendInputData(buf);
        emit EventSent(InputEventType::MouseMove);
        return true;
    }

    // ── Mouse Button ────────────────────────────────────────────────────
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        MouseButtonEvent ev;
        ev.button = QtButtonToButton(me->button());

        if (event->type() == QEvent::MouseButtonRelease) {
            ev.type = InputEventType::MouseButtonUp;
            SerializeMouseButton(ev, buf);
            SendInputData(buf);
        } else {
            // Press e DblClick: enviar Down + Up + Down (duplo clique = dois pares)
            ev.type = InputEventType::MouseButtonDown;
            SerializeMouseButton(ev, buf);
            SendInputData(buf);

            if (event->type() == QEvent::MouseButtonDblClick) {
                buf.clear();
                ev.type = InputEventType::MouseButtonUp;
                SerializeMouseButton(ev, buf);
                SendInputData(buf);

                buf.clear();
                ev.type = InputEventType::MouseButtonDown;
                SerializeMouseButton(ev, buf);
                SendInputData(buf);
            }
        }
        emit EventSent(ev.type);
        return true;
    }

    // ── Mouse Scroll ────────────────────────────────────────────────────
    case QEvent::Wheel: {
        auto* we = static_cast<QWheelEvent*>(event);
        QPoint delta = we->angleDelta();
        MouseScrollEvent ev;
        // angleDelta é em 1/8 de grau, 120 = 1 notch. Normalizar para ±notches.
        ev.delta_x = static_cast<int8_t>(std::clamp(delta.x() / 120, -127, 127));
        ev.delta_y = static_cast<int8_t>(std::clamp(delta.y() / 120, -127, 127));
        SerializeMouseScroll(ev, buf);
        SendInputData(buf);
        emit EventSent(InputEventType::MouseScroll);
        return true;
    }

    // ── Keyboard ────────────────────────────────────────────────────────
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto* ke = static_cast<QKeyEvent*>(event);

        // Ignorar auto-repeat para KeyDown (evitar flood).
        if (event->type() == QEvent::KeyPress && ke->isAutoRepeat())
            return true;

        KeyEvent ev;
        ev.type = (event->type() == QEvent::KeyPress)
                      ? InputEventType::KeyDown
                      : InputEventType::KeyUp;
        // Tentar obter o Virtual Key code real via nativeVirtualKey.
        // Se for zero (ex: Wayland no Linux), faz fallback pro mapeamento Qt.
        ev.vkey_code = ke->nativeVirtualKey();
        if (ev.vkey_code == 0) {
            ev.vkey_code = QtKeyToVirtualKey(ke->key());
        }

        ev.modifiers = QtModifiersToMask(ke->modifiers());

        // Só enviar se mapeou para um VK válido.
        if (ev.vkey_code != 0) {
            SerializeKey(ev, buf);
            SendInputData(buf);
            emit EventSent(ev.type);
            if (ev.vkey_code == VK_NUMLOCK && ev.type == InputEventType::KeyUp) {
                if (const auto num_lock_state = ReadLocalNumLockState(); num_lock_state.has_value()) {
                    emit NumLockStateChanged(*num_lock_state);
                }
            }
        }
        return true;
    }

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

// ─── Envio pelo Data Channel ────────────────────────────────────────────────

void InputCapture::SendInputData(const std::vector<uint8_t>& data)
{
    if (peer_ && !data.empty())
        peer_->SendDataChannelMessage(kInputChannel, data.data(), data.size(), true);
}

// ─── Conversão de coordenadas ───────────────────────────────────────────────

void InputCapture::WidgetPosToNormalized(const QPointF& pos,
                                          float& out_x, float& out_y) const
{
    if (!target_) {
        out_x = out_y = 0.0f;
        return;
    }

    float w = static_cast<float>(target_->width());
    float h = static_cast<float>(target_->height());

    // Calcular o rect real do vídeo dentro do widget (letterboxing),
    // replicando a mesma lógica do VideoRenderer::paintGL.
    float vp_x = 0.0f, vp_y = 0.0f, vp_w = w, vp_h = h;
    auto* renderer = qobject_cast<VideoRenderer*>(target_);
    if (renderer && renderer->DisplayWidth() > 0 && renderer->DisplayHeight() > 0) {
        float video_aspect  = static_cast<float>(renderer->DisplayWidth())
                            / static_cast<float>(renderer->DisplayHeight());
        float widget_aspect = (h > 0) ? w / h : 1.0f;

        if (widget_aspect > video_aspect) {
            vp_h = h;
            vp_w = h * video_aspect;
            vp_x = (w - vp_w) / 2.0f;
            vp_y = 0.0f;
        } else {
            vp_w = w;
            vp_h = w / video_aspect;
            vp_x = 0.0f;
            vp_y = (h - vp_h) / 2.0f;
        }
    }

    // Mapear coordenada do mouse para o espaço do vídeo.
    float local_x = static_cast<float>(pos.x()) - vp_x;
    float local_y = static_cast<float>(pos.y()) - vp_y;

    out_x = (vp_w > 0) ? std::clamp(local_x / vp_w, 0.0f, 1.0f) : 0.0f;
    out_y = (vp_h > 0) ? std::clamp(local_y / vp_h, 0.0f, 1.0f) : 0.0f;
}

// ─── Mapeamento Qt → Windows Virtual Key ────────────────────────────────────

uint16_t InputCapture::QtKeyToVirtualKey(int qt_key)
{
    // Mapeamento direto para letras e números (Qt::Key_A..Z == 0x41..0x5A = VK_A..VK_Z).
    if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z)
        return static_cast<uint16_t>(qt_key);  // Qt::Key_A == 0x41 == VK_A

    if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9)
        return static_cast<uint16_t>(qt_key);  // Qt::Key_0 == 0x30 == VK_0

    // F1–F24.
    if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F24)
        return static_cast<uint16_t>(VK_F1 + (qt_key - Qt::Key_F1));

    // Mapeamento de teclas especiais.
    switch (qt_key) {
        case Qt::Key_Escape:      return VK_ESCAPE;
        case Qt::Key_Tab:         return VK_TAB;
        case Qt::Key_Backspace:   return VK_BACK;
        case Qt::Key_Return:
        case Qt::Key_Enter:       return VK_RETURN;
        case Qt::Key_Insert:      return VK_INSERT;
        case Qt::Key_Delete:      return VK_DELETE;
        case Qt::Key_Home:        return VK_HOME;
        case Qt::Key_End:         return VK_END;
        case Qt::Key_PageUp:      return VK_PRIOR;
        case Qt::Key_PageDown:    return VK_NEXT;
        case Qt::Key_Left:        return VK_LEFT;
        case Qt::Key_Right:       return VK_RIGHT;
        case Qt::Key_Up:          return VK_UP;
        case Qt::Key_Down:        return VK_DOWN;
        case Qt::Key_Shift:       return VK_SHIFT;
        case Qt::Key_Control:     return VK_CONTROL;
        case Qt::Key_Alt:         return VK_MENU;
        case Qt::Key_Meta:        return VK_LWIN;
        case Qt::Key_CapsLock:    return VK_CAPITAL;
        case Qt::Key_NumLock:     return VK_NUMLOCK;
        case Qt::Key_ScrollLock:  return VK_SCROLL;
        case Qt::Key_Print:       return VK_SNAPSHOT;
        case Qt::Key_Pause:       return VK_PAUSE;
        case Qt::Key_Space:       return VK_SPACE;
        case Qt::Key_Minus:       return VK_OEM_MINUS;
        case Qt::Key_Equal:       return VK_OEM_PLUS;
        case Qt::Key_BracketLeft: return VK_OEM_4;
        case Qt::Key_BracketRight:return VK_OEM_6;
        case Qt::Key_Backslash:   return VK_OEM_5;
        case Qt::Key_Semicolon:   return VK_OEM_1;
        case Qt::Key_Apostrophe:  return VK_OEM_7;
        case Qt::Key_Comma:       return VK_OEM_COMMA;
        case Qt::Key_Period:      return VK_OEM_PERIOD;
        case Qt::Key_Slash:       return VK_OEM_2;
        case Qt::Key_QuoteLeft:   return VK_OEM_3;  // ` / ~
        default:                  return 0;
    }
}

uint16_t InputCapture::QtModifiersToMask(Qt::KeyboardModifiers mods)
{
    uint16_t mask = 0;
    if (mods & Qt::ShiftModifier)   mask |= ModShift;
    if (mods & Qt::ControlModifier) mask |= ModCtrl;
    if (mods & Qt::AltModifier)     mask |= ModAlt;
    if (mods & Qt::MetaModifier)    mask |= ModWin;
    return mask;
}

MouseButton InputCapture::QtButtonToButton(Qt::MouseButton button)
{
    switch (button) {
        case Qt::LeftButton:   return MouseButton::Left;
        case Qt::RightButton:  return MouseButton::Right;
        case Qt::MiddleButton: return MouseButton::Middle;
        default:               return MouseButton::Left;
    }
}

} // namespace remotex::viewer
