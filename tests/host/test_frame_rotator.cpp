#include <gtest/gtest.h>

#include "host/frame_rotator.h"

#include <array>
#include <vector>

namespace {

using remotex::host::FrameRotator;

constexpr uint32_t kWidth = 2;
constexpr uint32_t kHeight = 3;
constexpr uint32_t kPitch = kWidth * 4u;

std::vector<uint8_t> MakePattern() {
    std::vector<uint8_t> bytes(static_cast<size_t>(kPitch) * kHeight, 0);

    const std::array<std::array<uint8_t, 4>, 6> pixels{{
        {10, 20, 30, 255},
        {40, 50, 60, 255},
        {70, 80, 90, 255},
        {100, 110, 120, 255},
        {130, 140, 150, 255},
        {160, 170, 180, 255},
    }};

    for (size_t index = 0; index < pixels.size(); ++index) {
        const size_t offset = index * 4u;
        bytes[offset + 0] = pixels[index][0];
        bytes[offset + 1] = pixels[index][1];
        bytes[offset + 2] = pixels[index][2];
        bytes[offset + 3] = pixels[index][3];
    }

    return bytes;
}

std::array<uint8_t, 4> PixelAt(const uint8_t* data, uint32_t pitch, uint32_t x, uint32_t y) {
    const size_t offset = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u;
    return {data[offset + 0], data[offset + 1], data[offset + 2], data[offset + 3]};
}

ID3D11Device* FakeDevice() {
    return reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(1));
}

TEST(FrameRotator, IdentityRotationKeepsDimensionsAndPixelOrder) {
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(FakeDevice()));

    const auto pattern = MakePattern();
    ASSERT_TRUE(rotator.Rotate(pattern.data(), kWidth, kHeight, kPitch, DXGI_MODE_ROTATION_IDENTITY));
    ASSERT_NE(rotator.GetRotatedData(), nullptr);
    EXPECT_EQ(rotator.GetRotatedWidth(), kWidth);
    EXPECT_EQ(rotator.GetRotatedHeight(), kHeight);
    EXPECT_EQ(rotator.GetRotatedPitch(), kWidth * 4u);
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 0, 0),
              (std::array<uint8_t, 4>{10, 20, 30, 255}));
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 1, 2),
              (std::array<uint8_t, 4>{160, 170, 180, 255}));
}

TEST(FrameRotator, RightAngleRotationsSwapDimensionsAndMoveCorners) {
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(FakeDevice()));

    const auto pattern = MakePattern();
    ASSERT_TRUE(rotator.Rotate(pattern.data(), kWidth, kHeight, kPitch, DXGI_MODE_ROTATION_ROTATE90));
    EXPECT_EQ(rotator.GetRotatedWidth(), kHeight);
    EXPECT_EQ(rotator.GetRotatedHeight(), kWidth);
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 0, 0),
              (std::array<uint8_t, 4>{130, 140, 150, 255}));
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 2, 1),
              (std::array<uint8_t, 4>{40, 50, 60, 255}));

    ASSERT_TRUE(rotator.Rotate(pattern.data(), kWidth, kHeight, kPitch, DXGI_MODE_ROTATION_ROTATE270));
    EXPECT_EQ(rotator.GetRotatedWidth(), kHeight);
    EXPECT_EQ(rotator.GetRotatedHeight(), kWidth);
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 0, 0),
              (std::array<uint8_t, 4>{40, 50, 60, 255}));
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 2, 1),
              (std::array<uint8_t, 4>{130, 140, 150, 255}));
}

TEST(FrameRotator, Rotate180KeepsDimensionsAndFlipsCorners) {
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(FakeDevice()));

    const auto pattern = MakePattern();
    ASSERT_TRUE(rotator.Rotate(pattern.data(), kWidth, kHeight, kPitch, DXGI_MODE_ROTATION_ROTATE180));
    EXPECT_EQ(rotator.GetRotatedWidth(), kWidth);
    EXPECT_EQ(rotator.GetRotatedHeight(), kHeight);
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 0, 0),
              (std::array<uint8_t, 4>{160, 170, 180, 255}));
    EXPECT_EQ(PixelAt(rotator.GetRotatedData(), rotator.GetRotatedPitch(), 1, 2),
              (std::array<uint8_t, 4>{10, 20, 30, 255}));
}

} // namespace
