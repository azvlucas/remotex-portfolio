// RemoteX — core/input_protocol.cpp
// Serialização/deserialização binária de eventos de input remoto.

#include "core/input_protocol.h"

#include <algorithm>
#include <cstring>

namespace remotex::core {

// ─── Helpers de byte-order (little-endian no wire) ──────────────────────────

static void WriteU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}

static void WriteI8(std::vector<uint8_t>& out, int8_t v) {
    out.push_back(static_cast<uint8_t>(v));
}

static void WriteU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static uint8_t ReadU8(const uint8_t* p) {
    return *p;
}

static int8_t ReadI8(const uint8_t* p) {
    return static_cast<int8_t>(*p);
}

static uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// ─── Conversão normalizada ──────────────────────────────────────────────────

uint16_t NormalizedToUint16(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint16_t>(value * 65535.0f + 0.5f);
}

float Uint16ToNormalized(uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

// ─── Serialização ───────────────────────────────────────────────────────────

void SerializeMouseMove(const MouseMoveEvent& event, std::vector<uint8_t>& out) {
    out.reserve(out.size() + kMouseMoveSize);
    WriteU8(out, static_cast<uint8_t>(InputEventType::MouseMove));
    WriteU16LE(out, NormalizedToUint16(event.x));
    WriteU16LE(out, NormalizedToUint16(event.y));
}

void SerializeMouseButton(const MouseButtonEvent& event, std::vector<uint8_t>& out) {
    out.reserve(out.size() + kMouseButtonSize);
    WriteU8(out, static_cast<uint8_t>(event.type));
    WriteU8(out, static_cast<uint8_t>(event.button));
}

void SerializeMouseScroll(const MouseScrollEvent& event, std::vector<uint8_t>& out) {
    out.reserve(out.size() + kMouseScrollSize);
    WriteU8(out, static_cast<uint8_t>(InputEventType::MouseScroll));
    WriteI8(out, event.delta_x);
    WriteI8(out, event.delta_y);
}

void SerializeKey(const KeyEvent& event, std::vector<uint8_t>& out) {
    out.reserve(out.size() + kKeySize);
    WriteU8(out, static_cast<uint8_t>(event.type));
    WriteU16LE(out, event.vkey_code);
    WriteU16LE(out, event.modifiers);
}

void Serialize(const InputEvent& event, std::vector<uint8_t>& out) {
    switch (event.type) {
        case InputEventType::MouseMove:
            SerializeMouseMove(event.mouse_move, out);
            break;
        case InputEventType::MouseButtonDown:
        case InputEventType::MouseButtonUp:
            SerializeMouseButton(event.mouse_button, out);
            break;
        case InputEventType::MouseScroll:
            SerializeMouseScroll(event.mouse_scroll, out);
            break;
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
            SerializeKey(event.key, out);
            break;
    }
}

// ─── Deserialização ─────────────────────────────────────────────────────────

std::optional<DeserializeResult> Deserialize(std::span<const uint8_t> data) {
    if (data.empty())
        return std::nullopt;

    auto event_type = static_cast<InputEventType>(data[0]);
    DeserializeResult result;
    result.event.type = event_type;

    switch (event_type) {
        case InputEventType::MouseMove: {
            if (data.size() < kMouseMoveSize)
                return std::nullopt;
            result.event.mouse_move.x = Uint16ToNormalized(ReadU16LE(&data[1]));
            result.event.mouse_move.y = Uint16ToNormalized(ReadU16LE(&data[3]));
            result.bytes_consumed = kMouseMoveSize;
            break;
        }

        case InputEventType::MouseButtonDown:
        case InputEventType::MouseButtonUp: {
            if (data.size() < kMouseButtonSize)
                return std::nullopt;
            result.event.mouse_button.type = event_type;
            result.event.mouse_button.button = static_cast<MouseButton>(ReadU8(&data[1]));
            result.bytes_consumed = kMouseButtonSize;
            break;
        }

        case InputEventType::MouseScroll: {
            if (data.size() < kMouseScrollSize)
                return std::nullopt;
            result.event.mouse_scroll.delta_x = ReadI8(&data[1]);
            result.event.mouse_scroll.delta_y = ReadI8(&data[2]);
            result.bytes_consumed = kMouseScrollSize;
            break;
        }

        case InputEventType::KeyDown:
        case InputEventType::KeyUp: {
            if (data.size() < kKeySize)
                return std::nullopt;
            result.event.key.type = event_type;
            result.event.key.vkey_code = ReadU16LE(&data[1]);
            result.event.key.modifiers = ReadU16LE(&data[3]);
            result.bytes_consumed = kKeySize;
            break;
        }

        default:
            return std::nullopt;
    }

    return result;
}

} // namespace remotex::core
