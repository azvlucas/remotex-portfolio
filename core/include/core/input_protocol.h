#pragma once
// RemoteX — core/input_protocol.h
// Serialização/deserialização binária de eventos de input remoto.
// Protocolo compacto conforme seção 4.1 da arquitetura.
//
// Layout:
//   Mouse Move   (5 bytes): [0x01] [X:uint16] [Y:uint16]    — coords normalizadas 0–65535
//   Mouse Button (2 bytes): [0x02|0x03] [Button:uint8]       — 0=Left 1=Right 2=Middle
//   Mouse Scroll (3 bytes): [0x04] [DeltaX:int8] [DeltaY:int8]
//   Key Down/Up  (5 bytes): [0x05|0x06] [VKeyCode:uint16] [Modifiers:uint16]
//
// Coordenadas normalizadas: float 0.0–1.0 mapeado para uint16 0–65535.
// Isso dá ~0.0015% de resolução, suficiente para 8K (7680px → ~0.12px erro).

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace remotex::core {

// ─── Constantes do protocolo ────────────────────────────────────────────────

enum class InputEventType : uint8_t {
    MouseMove       = 0x01,
    MouseButtonDown = 0x02,
    MouseButtonUp   = 0x03,
    MouseScroll     = 0x04,
    KeyDown         = 0x05,
    KeyUp           = 0x06,
};

enum class MouseButton : uint8_t {
    Left   = 0,
    Right  = 1,
    Middle = 2,
};

// Modificadores de teclado (bitmask).
enum KeyModifier : uint16_t {
    ModShift = 1 << 0,
    ModCtrl  = 1 << 1,
    ModAlt   = 1 << 2,
    ModWin   = 1 << 3,
};

// ─── Tamanhos fixos de cada mensagem ────────────────────────────────────────

inline constexpr size_t kMouseMoveSize       = 5;  // tipo(1) + X(2) + Y(2)
inline constexpr size_t kMouseButtonSize     = 2;  // tipo(1) + button(1)
inline constexpr size_t kMouseScrollSize     = 3;  // tipo(1) + dX(1) + dY(1)
inline constexpr size_t kKeySize             = 5;  // tipo(1) + vkey(2) + mods(2)

// ─── Estruturas de eventos ──────────────────────────────────────────────────

struct MouseMoveEvent {
    float x = 0.0f;  // 0.0 – 1.0
    float y = 0.0f;  // 0.0 – 1.0
};

struct MouseButtonEvent {
    InputEventType type   = InputEventType::MouseButtonDown;
    MouseButton    button = MouseButton::Left;
};

struct MouseScrollEvent {
    int8_t delta_x = 0;
    int8_t delta_y = 0;
};

struct KeyEvent {
    InputEventType type      = InputEventType::KeyDown;
    uint16_t       vkey_code = 0;
    uint16_t       modifiers = 0;  // Bitmask de KeyModifier
};

// Evento genérico que pode ser qualquer um dos tipos acima.
struct InputEvent {
    InputEventType type;
    union {
        MouseMoveEvent   mouse_move;
        MouseButtonEvent mouse_button;
        MouseScrollEvent mouse_scroll;
        KeyEvent         key;
    };

    InputEvent() : type(InputEventType::MouseMove), mouse_move{} {}
};

// ─── Serialização ───────────────────────────────────────────────────────────

// Serializa um MouseMoveEvent em exatamente kMouseMoveSize bytes.
void SerializeMouseMove(const MouseMoveEvent& event, std::vector<uint8_t>& out);

// Serializa um MouseButtonEvent (down ou up).
void SerializeMouseButton(const MouseButtonEvent& event, std::vector<uint8_t>& out);

// Serializa um MouseScrollEvent.
void SerializeMouseScroll(const MouseScrollEvent& event, std::vector<uint8_t>& out);

// Serializa um KeyEvent (down ou up).
void SerializeKey(const KeyEvent& event, std::vector<uint8_t>& out);

// Serializa qualquer InputEvent.
void Serialize(const InputEvent& event, std::vector<uint8_t>& out);

// ─── Deserialização ─────────────────────────────────────────────────────────

// Deserializa um InputEvent a partir de um buffer.
// Retorna o evento e o número de bytes consumidos, ou std::nullopt se inválido.
struct DeserializeResult {
    InputEvent event;
    size_t     bytes_consumed = 0;
};

std::optional<DeserializeResult> Deserialize(std::span<const uint8_t> data);

// ─── Helpers de conversão ───────────────────────────────────────────────────

// Converte float normalizado (0.0–1.0) para uint16 (0–65535).
uint16_t NormalizedToUint16(float value);

// Converte uint16 (0–65535) para float normalizado (0.0–1.0).
float Uint16ToNormalized(uint16_t value);

} // namespace remotex::core
