#pragma once
// RemoteX — viewer/input_capture.h
// Captura eventos de mouse/teclado da janela Qt de sessão remota,
// serializa via InputProtocol e envia pelo Data Channel "input".
//
// Usa WH_KEYBOARD_LL hook para interceptar teclas de sistema (Win, Alt+Tab)
// e redirecioná-las para a máquina remota quando a janela de sessão tem foco.

#include "core/input_protocol.h"
#include "core/webrtc_peer.h"

#include <QObject>
#include <QWidget>
#include <QEvent>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <vector>

class InputCaptureTest;

namespace remotex::viewer {

// InputCapture é instalado como event filter num QWidget de sessão remota.
// Captura mouse e teclado, normaliza coordenadas, serializa e envia via WebRTC.
//
// Quando habilitado e a janela tem foco, instala um low-level keyboard hook
// para interceptar atalhos de sistema (Win, Win+E, Alt+Tab, etc.) e enviá-los
// para a máquina remota em vez de executar localmente.
//
// Uso:
//   InputCapture capture(&webrtcPeer, remoteSessionWidget);
//   capture.SetEnabled(true);
//
// O widget deve ter setMouseTracking(true) e setFocusPolicy(Qt::StrongFocus).
class InputCapture : public QObject {
    Q_OBJECT

public:
    // webrtc_peer: peer pelo qual os eventos serão enviados (data channel "input").
    // target: widget da sessão remota cujos eventos serão capturados.
    explicit InputCapture(remotex::core::WebRTCPeer* webrtc_peer,
                          QWidget* target,
                          QObject* parent = nullptr);

    ~InputCapture() override;

    // Habilita/desabilita a captura de input.
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_; }

    // Define a resolução do stream remoto (para cálculo de coordenadas).
    void SetRemoteResolution(int width, int height);

signals:
    // Emitido quando um evento é capturado e enviado com sucesso.
    void EventSent(remotex::core::InputEventType type);
    void NumLockStateChanged(bool enabled);

protected:
    // Event filter instalado no widget alvo.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Envia um buffer serializado pelo data channel "input".
    void SendInputData(const std::vector<uint8_t>& data);

    // Converte posição do mouse no widget para coordenadas normalizadas.
    void WidgetPosToNormalized(const QPointF& pos, float& out_x, float& out_y) const;

    // Permitir acesso do gtest às funções utilitárias estáticas
    friend class ::InputCaptureTest;

    // Converte Qt::Key + Qt modifiers para VK code + nossos modifiers.
    static uint16_t QtKeyToVirtualKey(int qt_key);
    static uint16_t QtModifiersToMask(Qt::KeyboardModifiers mods);

    // Converte Qt::MouseButton para nosso enum.
    static remotex::core::MouseButton QtButtonToButton(Qt::MouseButton button);

    // ── Low-level keyboard hook (teclas de sistema) ──────────────────────
    void InstallKeyboardHook();
    void RemoveKeyboardHook();
    void UpdateHookState();   // instala/remove hook conforme enabled_ + foco

    // Callback estático do hook — despacha para a instância ativa.
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // Processa um evento do hook. Retorna true se o evento foi consumido.
    bool HandleHookedKey(DWORD vk_code, bool key_up, bool is_extended);

    // Verifica se uma VK é tecla de sistema que devemos interceptar.
    static bool IsSystemKey(DWORD vk_code);
    static std::optional<bool> ReadLocalNumLockState();

    remotex::core::WebRTCPeer* peer_       = nullptr;
    QWidget*                   target_     = nullptr;
    bool                       enabled_    = false;
    int                        remote_w_   = 1920;
    int                        remote_h_   = 1080;

    HHOOK keyboard_hook_  = nullptr;   // Handle do WH_KEYBOARD_LL hook

    // Instância ativa — apenas uma InputCapture pode ter o hook por vez.
    static InputCapture* s_active_instance_;
};

} // namespace remotex::viewer
