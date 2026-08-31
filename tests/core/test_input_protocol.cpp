// RemoteX — tests/core/test_input_protocol.cpp
// Testes unitários para serialização/deserialização do protocolo de input.

#include <gtest/gtest.h>
#include "core/input_protocol.h"

#include <cmath>

using namespace remotex::core;

// ─── NormalizedToUint16 / Uint16ToNormalized ────────────────────────────────

TEST(InputProtocol, NormalizedConversion_Zero) {
    EXPECT_EQ(NormalizedToUint16(0.0f), 0);
    EXPECT_FLOAT_EQ(Uint16ToNormalized(0), 0.0f);
}

TEST(InputProtocol, NormalizedConversion_One) {
    EXPECT_EQ(NormalizedToUint16(1.0f), 65535);
    EXPECT_FLOAT_EQ(Uint16ToNormalized(65535), 1.0f);
}

TEST(InputProtocol, NormalizedConversion_Mid) {
    uint16_t mid = NormalizedToUint16(0.5f);
    EXPECT_NEAR(mid, 32768, 1);
    float back = Uint16ToNormalized(mid);
    EXPECT_NEAR(back, 0.5f, 0.001f);
}

TEST(InputProtocol, NormalizedConversion_Clamp) {
    EXPECT_EQ(NormalizedToUint16(-1.0f), 0);
    EXPECT_EQ(NormalizedToUint16(2.0f), 65535);
}

TEST(InputProtocol, NormalizedConversion_RoundTrip) {
    for (float v : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
        uint16_t encoded = NormalizedToUint16(v);
        float decoded = Uint16ToNormalized(encoded);
        EXPECT_NEAR(decoded, v, 0.0001f) << "Roundtrip failed for " << v;
    }
}

// ─── MouseMove ──────────────────────────────────────────────────────────────

TEST(InputProtocol, SerializeDeserialize_MouseMove) {
    MouseMoveEvent event{0.3f, 0.7f};
    std::vector<uint8_t> buf;
    SerializeMouseMove(event, buf);

    EXPECT_EQ(buf.size(), kMouseMoveSize);
    EXPECT_EQ(buf[0], static_cast<uint8_t>(InputEventType::MouseMove));

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bytes_consumed, kMouseMoveSize);
    EXPECT_EQ(result->event.type, InputEventType::MouseMove);
    EXPECT_NEAR(result->event.mouse_move.x, 0.3f, 0.001f);
    EXPECT_NEAR(result->event.mouse_move.y, 0.7f, 0.001f);
}

TEST(InputProtocol, MouseMove_Corners) {
    for (auto [x, y] : std::initializer_list<std::pair<float, float>>
            {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}}) {
        MouseMoveEvent event{x, y};
        std::vector<uint8_t> buf;
        SerializeMouseMove(event, buf);

        auto result = Deserialize(buf);
        ASSERT_TRUE(result.has_value());
        EXPECT_NEAR(result->event.mouse_move.x, x, 0.0001f);
        EXPECT_NEAR(result->event.mouse_move.y, y, 0.0001f);
    }
}

// ─── MouseButton ────────────────────────────────────────────────────────────

TEST(InputProtocol, SerializeDeserialize_MouseButtonDown) {
    MouseButtonEvent event{InputEventType::MouseButtonDown, MouseButton::Left};
    std::vector<uint8_t> buf;
    SerializeMouseButton(event, buf);

    EXPECT_EQ(buf.size(), kMouseButtonSize);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bytes_consumed, kMouseButtonSize);
    EXPECT_EQ(result->event.type, InputEventType::MouseButtonDown);
    EXPECT_EQ(result->event.mouse_button.button, MouseButton::Left);
}

TEST(InputProtocol, SerializeDeserialize_MouseButtonUp) {
    MouseButtonEvent event{InputEventType::MouseButtonUp, MouseButton::Right};
    std::vector<uint8_t> buf;
    SerializeMouseButton(event, buf);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->event.type, InputEventType::MouseButtonUp);
    EXPECT_EQ(result->event.mouse_button.button, MouseButton::Right);
}

TEST(InputProtocol, SerializeDeserialize_MouseButtonMiddle) {
    MouseButtonEvent event{InputEventType::MouseButtonDown, MouseButton::Middle};
    std::vector<uint8_t> buf;
    SerializeMouseButton(event, buf);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->event.mouse_button.button, MouseButton::Middle);
}

// ─── MouseScroll ────────────────────────────────────────────────────────────

TEST(InputProtocol, SerializeDeserialize_MouseScroll) {
    MouseScrollEvent event{0, -3};
    std::vector<uint8_t> buf;
    SerializeMouseScroll(event, buf);

    EXPECT_EQ(buf.size(), kMouseScrollSize);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bytes_consumed, kMouseScrollSize);
    EXPECT_EQ(result->event.type, InputEventType::MouseScroll);
    EXPECT_EQ(result->event.mouse_scroll.delta_x, 0);
    EXPECT_EQ(result->event.mouse_scroll.delta_y, -3);
}

TEST(InputProtocol, MouseScroll_PositiveAndNegative) {
    for (int8_t dx : {-127, -1, 0, 1, 127}) {
        for (int8_t dy : {-127, -1, 0, 1, 127}) {
            MouseScrollEvent event{dx, dy};
            std::vector<uint8_t> buf;
            SerializeMouseScroll(event, buf);

            auto result = Deserialize(buf);
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result->event.mouse_scroll.delta_x, dx);
            EXPECT_EQ(result->event.mouse_scroll.delta_y, dy);
        }
    }
}

// ─── Key ────────────────────────────────────────────────────────────────────

TEST(InputProtocol, SerializeDeserialize_KeyDown) {
    KeyEvent event{InputEventType::KeyDown, 0x41, ModCtrl | ModShift};  // Ctrl+Shift+A
    std::vector<uint8_t> buf;
    SerializeKey(event, buf);

    EXPECT_EQ(buf.size(), kKeySize);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bytes_consumed, kKeySize);
    EXPECT_EQ(result->event.type, InputEventType::KeyDown);
    EXPECT_EQ(result->event.key.vkey_code, 0x41);
    EXPECT_EQ(result->event.key.modifiers, ModCtrl | ModShift);
}

TEST(InputProtocol, SerializeDeserialize_KeyUp) {
    KeyEvent event{InputEventType::KeyUp, 0x0D, 0};  // Enter, no mods
    std::vector<uint8_t> buf;
    SerializeKey(event, buf);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->event.type, InputEventType::KeyUp);
    EXPECT_EQ(result->event.key.vkey_code, 0x0D);
    EXPECT_EQ(result->event.key.modifiers, 0);
}

// ─── Generic Serialize ──────────────────────────────────────────────────────

TEST(InputProtocol, GenericSerialize_MouseMove) {
    InputEvent event;
    event.type = InputEventType::MouseMove;
    event.mouse_move = {0.5f, 0.5f};

    std::vector<uint8_t> buf;
    Serialize(event, buf);
    EXPECT_EQ(buf.size(), kMouseMoveSize);

    auto result = Deserialize(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->event.type, InputEventType::MouseMove);
}

// ─── Multiple events in buffer ──────────────────────────────────────────────

TEST(InputProtocol, MultipleEventsInBuffer) {
    std::vector<uint8_t> buf;

    MouseMoveEvent move{0.1f, 0.2f};
    SerializeMouseMove(move, buf);

    KeyEvent key{InputEventType::KeyDown, 0x42, ModAlt};
    SerializeKey(key, buf);

    EXPECT_EQ(buf.size(), kMouseMoveSize + kKeySize);

    // Deserialize first.
    auto r1 = Deserialize(std::span<const uint8_t>(buf));
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->event.type, InputEventType::MouseMove);
    EXPECT_EQ(r1->bytes_consumed, kMouseMoveSize);

    // Deserialize second.
    auto remaining = std::span<const uint8_t>(buf).subspan(r1->bytes_consumed);
    auto r2 = Deserialize(remaining);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->event.type, InputEventType::KeyDown);
    EXPECT_EQ(r2->event.key.vkey_code, 0x42);
}

// ─── Edge cases ─────────────────────────────────────────────────────────────

TEST(InputProtocol, Deserialize_EmptyBuffer) {
    std::vector<uint8_t> empty;
    auto result = Deserialize(empty);
    EXPECT_FALSE(result.has_value());
}

TEST(InputProtocol, Deserialize_TruncatedMouseMove) {
    std::vector<uint8_t> buf = {0x01, 0x00};  // Only 2 bytes, need 5
    auto result = Deserialize(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(InputProtocol, Deserialize_TruncatedKey) {
    std::vector<uint8_t> buf = {0x05, 0x41, 0x00};  // Only 3 bytes, need 5
    auto result = Deserialize(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(InputProtocol, Deserialize_InvalidEventType) {
    std::vector<uint8_t> buf = {0xFF};
    auto result = Deserialize(buf);
    EXPECT_FALSE(result.has_value());
}
