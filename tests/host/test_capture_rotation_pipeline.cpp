#include <gtest/gtest.h>

#include "host/capture_damage.h"
#include "host/capture_frame_preparer.h"
#include "host/frame_rotator.h"
#include "host/rotation_geometry.h"
#include "host/screen_capture.h"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace {

using remotex::host::CaptureDamageDecision;
using remotex::host::CaptureUpdateKind;
using remotex::host::CapturedFrame;
using remotex::host::ClassifyCaptureDamage;
using remotex::host::CopyRegion;
using remotex::host::DirtyRect;
using remotex::host::FrameRotator;
using remotex::host::MappedFrameView;
using remotex::host::PrepareCaptureFrame;
using remotex::host::PointerSnapshot;
using remotex::host::RotatePoint;
using remotex::host::RotateMoveRect;
using remotex::host::RotatePointerDrawOrigin;
using remotex::host::RotateRegion;

struct PipelineFrame {
    std::vector<uint8_t> storage;
    const uint8_t* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    bool used_rotator = false;
    std::vector<CopyRegion> logical_dirty_regions;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> logical_move_rects;
    std::optional<CopyRegion> logical_pointer_region;
    std::optional<POINT> logical_pointer_position;
};

Microsoft::WRL::ComPtr<ID3D11Device> CreateWarpDevice() {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    const HRESULT hr = D3D11CreateDevice(nullptr,
                                         D3D_DRIVER_TYPE_WARP,
                                         nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         feature_levels,
                                         static_cast<UINT>(std::size(feature_levels)),
                                         D3D11_SDK_VERSION,
                                         device.GetAddressOf(),
                                         &feature_level,
                                         context.GetAddressOf());
    EXPECT_TRUE(SUCCEEDED(hr));
    return device;
}

std::vector<uint8_t> MakePattern(uint32_t width, uint32_t height) {
    const uint32_t pitch = width * 4u;
    std::vector<uint8_t> bytes(static_cast<size_t>(pitch) * height, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u;
            bytes[offset + 0] = static_cast<uint8_t>(10 + y * 20 + x);
            bytes[offset + 1] = static_cast<uint8_t>(40 + y * 20 + x);
            bytes[offset + 2] = static_cast<uint8_t>(70 + y * 20 + x);
            bytes[offset + 3] = 255;
        }
    }
    return bytes;
}

std::array<uint8_t, 4> PixelAt(const uint8_t* data, uint32_t pitch, uint32_t x, uint32_t y) {
    const size_t offset = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u;
    return {
        data[offset + 0],
        data[offset + 1],
        data[offset + 2],
        data[offset + 3],
    };
}

void CopyRegionPixels(const uint8_t* src,
                      uint32_t src_pitch,
                      std::vector<uint8_t>& dst,
                      uint32_t dst_pitch,
                      const CopyRegion& region) {
    const size_t row_bytes = static_cast<size_t>(region.right - region.left) * 4u;
    for (LONG y = region.top; y < region.bottom; ++y) {
        const size_t src_offset = static_cast<size_t>(y) * src_pitch +
                                  static_cast<size_t>(region.left) * 4u;
        const size_t dst_offset = static_cast<size_t>(y) * dst_pitch +
                                  static_cast<size_t>(region.left) * 4u;
        std::memcpy(dst.data() + dst_offset, src + src_offset, row_bytes);
    }
}

std::vector<uint8_t> ApplyRotatedIncrementalContract(const PipelineFrame& frame,
                                                     const std::vector<uint8_t>& previous) {
    std::vector<uint8_t> next = previous;
    for (const auto& region : frame.logical_dirty_regions) {
        CopyRegionPixels(frame.data, frame.pitch, next, frame.pitch, region);
    }
    if (frame.logical_pointer_region.has_value()) {
        CopyRegionPixels(frame.data, frame.pitch, next, frame.pitch, *frame.logical_pointer_region);
    }
    return next;
}

PipelineFrame RunPostPreparationBranch(const std::vector<uint8_t>& prepared_bgra,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t pitch,
                                       DXGI_MODE_ROTATION rotation,
                                       CaptureDamageDecision decision,
                                       const CapturedFrame& frame,
                                       FrameRotator& rotator,
                                       int& rotate_calls) {
    PipelineFrame output{
        .data = prepared_bgra.data(),
        .width = width,
        .height = height,
        .pitch = pitch,
        .used_rotator = false,
    };

    const bool rotate_frame =
        rotation == DXGI_MODE_ROTATION_ROTATE90 ||
        rotation == DXGI_MODE_ROTATION_ROTATE180 ||
        rotation == DXGI_MODE_ROTATION_ROTATE270;
    if (!rotate_frame) {
        return output;
    }

    decision.overlay_pointer = false;
    ++rotate_calls;
    EXPECT_TRUE(rotator.Rotate(prepared_bgra.data(), width, height, pitch, rotation));
    output.storage.assign(
        rotator.GetRotatedData(),
        rotator.GetRotatedData() + static_cast<size_t>(rotator.GetRotatedPitch()) * rotator.GetRotatedHeight());
    output.data = output.storage.data();
    output.width = rotator.GetRotatedWidth();
    output.height = rotator.GetRotatedHeight();
    output.pitch = rotator.GetRotatedPitch();
    output.used_rotator = true;

    for (const auto& copied_region : remotex::host::CollectCopiedRegions(decision, frame)) {
        output.logical_dirty_regions.push_back(
            RotateRegion(copied_region,
                         frame.width,
                         frame.height,
                         rotation));
    }

    for (const auto& move_rect : frame.move_rects) {
        output.logical_move_rects.push_back(
            RotateMoveRect(move_rect, frame.width, frame.height, rotation));
    }

    if (frame.pointer_visible) {
        const LONG draw_height =
            frame.pointer_shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
                ? static_cast<LONG>(frame.pointer_shape_info.Height / 2)
                : static_cast<LONG>(frame.pointer_shape_info.Height);
        const POINT logical_pointer = RotatePointerDrawOrigin(
            POINT{frame.pointer_x, frame.pointer_y},
            frame.pointer_shape_info.HotSpot,
            frame.pointer_shape_info.Width,
            static_cast<uint32_t>(draw_height),
            frame.width,
            frame.height,
            rotation);
        output.logical_pointer_position = logical_pointer;
        const CopyRegion logical_pointer_region = remotex::host::ClipRegion(
            CopyRegion{
                .left = logical_pointer.x,
                .top = logical_pointer.y,
                .right = logical_pointer.x + static_cast<LONG>(frame.pointer_shape_info.Width),
                .bottom = logical_pointer.y + draw_height,
            },
            output.width,
            output.height);
        if (remotex::host::IsValidRegion(logical_pointer_region)) {
            output.logical_pointer_region = logical_pointer_region;
        }
        remotex::host::ScreenCapture::OverlayPointerShape(
            std::span<uint8_t>(output.storage.data(), output.storage.size()),
            output.pitch,
            output.width,
            output.height,
            *output.logical_pointer_position,
            frame.pointer_shape_info,
            std::span<const uint8_t>(frame.pointer_shape_buffer.data(), frame.pointer_shape_buffer.size()));
    }

    return output;
}

TEST(CaptureRotationPipeline, RotateAfterPointerOverlayKeepsCursorPixelCorrect) {
    constexpr uint32_t kWidth = 3;
    constexpr uint32_t kHeight = 4;
    constexpr uint32_t kPitch = kWidth * 4u;
    const std::array<std::array<uint8_t, 4>, 4> cursor_pixels{{
        std::array<uint8_t, 4>{0x10, 0x20, 0x30, 0xFF},
        std::array<uint8_t, 4>{0x40, 0x50, 0x60, 0xFF},
        std::array<uint8_t, 4>{0x70, 0x80, 0x90, 0xFF},
        std::array<uint8_t, 4>{0xA0, 0xB0, 0xC0, 0xFF},
    }};

    std::vector<uint8_t> cached(static_cast<size_t>(kPitch) * kHeight, 0);
    const std::vector<uint8_t> mapped = MakePattern(kWidth, kHeight);

    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.pointer_visible = true;
    frame.pointer_x = 1;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 2;
    frame.pointer_shape_info.Height = 2;
    frame.pointer_shape_info.Pitch = 8;
    frame.pointer_shape_info.HotSpot.x = 1;
    frame.pointer_shape_info.HotSpot.y = 1;
    for (const auto& pixel : cursor_pixels) {
        frame.pointer_shape_buffer.insert(
            frame.pointer_shape_buffer.end(),
            pixel.begin(),
            pixel.end());
    }

    const CaptureDamageDecision decision = ClassifyCaptureDamage({
        .frame = &frame,
    });
    ASSERT_TRUE(decision.overlay_pointer);
    ASSERT_TRUE(decision.current_pointer_region.has_value());

    PrepareCaptureFrame(
        decision,
        MappedFrameView{
            .data = mapped.data(),
            .row_pitch = kPitch,
            .width = kWidth,
            .height = kHeight,
        },
        frame,
        cached,
        kPitch);

    const auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(device.Get()));

    int rotate_calls = 0;
    const PipelineFrame output = RunPostPreparationBranch(
        cached,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_ROTATE90,
        decision,
        frame,
        rotator,
        rotate_calls);

    ASSERT_TRUE(output.used_rotator);
    ASSERT_EQ(rotate_calls, 1);
    ASSERT_EQ(output.width, kHeight);
    ASSERT_EQ(output.height, kWidth);
    ASSERT_TRUE(output.logical_pointer_position.has_value());

    const POINT rotated_pointer = RotatePointerDrawOrigin(POINT{frame.pointer_x, frame.pointer_y},
                                                          frame.pointer_shape_info.HotSpot,
                                                          frame.pointer_shape_info.Width,
                                                          frame.pointer_shape_info.Height,
                                                          frame.width,
                                                          frame.height,
                                                          DXGI_MODE_ROTATION_ROTATE90);
    EXPECT_EQ(output.logical_pointer_position->x, rotated_pointer.x);
    EXPECT_EQ(output.logical_pointer_position->y, rotated_pointer.y);
    EXPECT_EQ(PixelAt(output.data, output.pitch, rotated_pointer.x, rotated_pointer.y), cursor_pixels[0]);
    EXPECT_EQ(PixelAt(output.data, output.pitch, rotated_pointer.x + 1, rotated_pointer.y), cursor_pixels[1]);
    EXPECT_EQ(PixelAt(output.data, output.pitch, rotated_pointer.x, rotated_pointer.y + 1), cursor_pixels[2]);
    EXPECT_EQ(PixelAt(output.data, output.pitch, rotated_pointer.x + 1, rotated_pointer.y + 1), cursor_pixels[3]);
    EXPECT_TRUE(output.logical_pointer_region.has_value());
    EXPECT_EQ(output.logical_pointer_region->left, rotated_pointer.x);
    EXPECT_EQ(output.logical_pointer_region->top, rotated_pointer.y);
    EXPECT_EQ(output.logical_pointer_region->right,
              rotated_pointer.x + static_cast<LONG>(frame.pointer_shape_info.Width));
    EXPECT_EQ(output.logical_pointer_region->bottom,
              rotated_pointer.y + static_cast<LONG>(frame.pointer_shape_info.Height));
}

TEST(CaptureRotationPipeline, RotatedCursorCompositionUsesHotspotAwareLogicalAnchor) {
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 3;
    constexpr uint32_t kPitch = kWidth * 4u;

    std::vector<uint8_t> cached = MakePattern(kWidth, kHeight);
    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.pointer_visible = true;
    frame.pointer_x = 0;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 2;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 8;
    frame.pointer_shape_info.HotSpot.x = 0;
    frame.pointer_shape_info.HotSpot.y = 0;
    frame.pointer_shape_buffer = {
        0x11, 0x22, 0x33, 0xFF,
        0x44, 0x55, 0x66, 0xFF,
    };

    CaptureDamageDecision decision;
    decision.current_pointer_region = CopyRegion{
        .left = frame.pointer_x,
        .top = frame.pointer_y,
        .right = frame.pointer_x + 2,
        .bottom = frame.pointer_y + 1,
    };

    const auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(device.Get()));

    int rotate_calls = 0;
    const PipelineFrame output = RunPostPreparationBranch(
        cached,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_ROTATE180,
        decision,
        frame,
        rotator,
        rotate_calls);

    ASSERT_TRUE(output.used_rotator);
    ASSERT_EQ(rotate_calls, 1);
    ASSERT_TRUE(output.logical_pointer_position.has_value());
    const POINT rotated_pointer = RotatePointerDrawOrigin(POINT{frame.pointer_x, frame.pointer_y},
                                                          frame.pointer_shape_info.HotSpot,
                                                          frame.pointer_shape_info.Width,
                                                          frame.pointer_shape_info.Height,
                                                          frame.width,
                                                          frame.height,
                                                          DXGI_MODE_ROTATION_ROTATE180);
    const POINT rotated_hotspot = RotatePoint(POINT{frame.pointer_x, frame.pointer_y},
                                              frame.width,
                                              frame.height,
                                              DXGI_MODE_ROTATION_ROTATE180);
    EXPECT_EQ(output.logical_pointer_position->x, rotated_pointer.x);
    EXPECT_EQ(output.logical_pointer_position->y, rotated_pointer.y);
    EXPECT_NE(rotated_pointer.x, rotated_hotspot.x);
    EXPECT_EQ(rotated_pointer.y, rotated_hotspot.y);
    EXPECT_EQ(PixelAt(output.data, output.pitch, rotated_pointer.x, rotated_pointer.y),
              (std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0xFF}));
    ASSERT_TRUE(output.logical_pointer_region.has_value());
    EXPECT_EQ(output.logical_pointer_region->left, rotated_pointer.x);
    EXPECT_EQ(output.logical_pointer_region->top, rotated_pointer.y);
    EXPECT_EQ(output.logical_pointer_region->right, static_cast<LONG>(output.width));
    EXPECT_EQ(output.logical_pointer_region->bottom,
              rotated_pointer.y + 1);
}

TEST(CaptureRotationPipeline, RotatedCursorCompositionMatchesHotspotAwareAnchorForAllRotations) {
    constexpr uint32_t kWidth = 5;
    constexpr uint32_t kHeight = 4;
    constexpr uint32_t kPitch = kWidth * 4u;

    for (const auto rotation : {DXGI_MODE_ROTATION_ROTATE90,
                                DXGI_MODE_ROTATION_ROTATE180,
                                DXGI_MODE_ROTATION_ROTATE270}) {
        std::vector<uint8_t> cached = MakePattern(kWidth, kHeight);
        CapturedFrame frame;
        frame.width = kWidth;
        frame.height = kHeight;
        frame.pointer_visible = true;
        frame.pointer_x = 2;
        frame.pointer_y = 1;
        frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
        frame.pointer_shape_info.Width = 3;
        frame.pointer_shape_info.Height = 2;
        frame.pointer_shape_info.Pitch = 12;
        frame.pointer_shape_info.HotSpot.x = 1;
        frame.pointer_shape_info.HotSpot.y = 1;
        frame.pointer_shape_buffer = {
            0x01, 0x02, 0x03, 0xFF, 0x11, 0x12, 0x13, 0xFF, 0x21, 0x22, 0x23, 0xFF,
            0x31, 0x32, 0x33, 0xFF, 0x41, 0x42, 0x43, 0xFF, 0x51, 0x52, 0x53, 0xFF,
        };

        CaptureDamageDecision decision;
        decision.current_pointer_region = CopyRegion{
            .left = frame.pointer_x - frame.pointer_shape_info.HotSpot.x,
            .top = frame.pointer_y - frame.pointer_shape_info.HotSpot.y,
            .right = frame.pointer_x - frame.pointer_shape_info.HotSpot.x +
                static_cast<LONG>(frame.pointer_shape_info.Width),
            .bottom = frame.pointer_y - frame.pointer_shape_info.HotSpot.y +
                static_cast<LONG>(frame.pointer_shape_info.Height),
        };

        const auto device = CreateWarpDevice();
        ASSERT_NE(device, nullptr);
        FrameRotator rotator;
        ASSERT_TRUE(rotator.Initialize(device.Get()));

        int rotate_calls = 0;
        const PipelineFrame output = RunPostPreparationBranch(
            cached,
            kWidth,
            kHeight,
            kPitch,
            rotation,
            decision,
            frame,
            rotator,
            rotate_calls);

        SCOPED_TRACE(static_cast<int>(rotation));
        ASSERT_TRUE(output.used_rotator);
        ASSERT_EQ(rotate_calls, 1);
        ASSERT_TRUE(output.logical_pointer_position.has_value());
        ASSERT_TRUE(output.logical_pointer_region.has_value());

        const POINT expected_anchor = RotatePointerDrawOrigin(POINT{frame.pointer_x, frame.pointer_y},
                                                              frame.pointer_shape_info.HotSpot,
                                                              frame.pointer_shape_info.Width,
                                                              frame.pointer_shape_info.Height,
                                                              frame.width,
                                                              frame.height,
                                                              rotation);
        EXPECT_EQ(output.logical_pointer_position->x, expected_anchor.x);
        EXPECT_EQ(output.logical_pointer_position->y, expected_anchor.y);
        EXPECT_EQ(output.logical_pointer_region->left, expected_anchor.x);
        EXPECT_EQ(output.logical_pointer_region->top, expected_anchor.y);
        EXPECT_EQ(PixelAt(output.data, output.pitch, expected_anchor.x, expected_anchor.y),
                  (std::array<uint8_t, 4>{0x01, 0x02, 0x03, 0xFF}));
    }
}

TEST(CaptureRotationPipeline, IdentityRotationBypassesRotator) {
    constexpr uint32_t kWidth = 3;
    constexpr uint32_t kHeight = 2;
    constexpr uint32_t kPitch = kWidth * 4u;

    const std::vector<uint8_t> prepared = MakePattern(kWidth, kHeight);
    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;

    const CaptureDamageDecision decision = ClassifyCaptureDamage({
        .force_first_frame = true,
        .frame = &frame,
    });

    const auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(device.Get()));

    int rotate_calls = 0;
    const PipelineFrame output = RunPostPreparationBranch(
        prepared,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_IDENTITY,
        decision,
        frame,
        rotator,
        rotate_calls);

    EXPECT_FALSE(output.used_rotator);
    EXPECT_EQ(rotate_calls, 0);
    EXPECT_EQ(output.data, prepared.data());
    EXPECT_EQ(output.width, kWidth);
    EXPECT_EQ(output.height, kHeight);
    EXPECT_EQ(output.pitch, kPitch);
}

TEST(CaptureRotationPipeline, RotationDoesNotForceFullRefreshDecision) {
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 3;
    constexpr uint32_t kPitch = kWidth * 4u;

    std::vector<uint8_t> cached = MakePattern(kWidth, kHeight);
    std::vector<uint8_t> mapped = MakePattern(kWidth, kHeight);

    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.dirty_rects.push_back(DirtyRect{1, 1, 2, 1});
    frame.move_rects.push_back(DXGI_OUTDUPL_MOVE_RECT{
        .SourcePoint = POINT{0, 0},
        .DestinationRect = RECT{1, 0, 3, 1},
    });

    const CaptureDamageDecision decision = ClassifyCaptureDamage({
        .frame = &frame,
    });
    ASSERT_EQ(decision.kind, CaptureUpdateKind::PartialUpdate);
    ASSERT_FALSE(decision.copy_full_frame);
    ASSERT_TRUE(decision.replay_move_rects);
    ASSERT_TRUE(decision.copy_dirty_regions);

    PrepareCaptureFrame(
        decision,
        MappedFrameView{
            .data = mapped.data(),
            .row_pitch = kPitch,
            .width = kWidth,
            .height = kHeight,
        },
        frame,
        cached,
        kPitch);

    const auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(device.Get()));

    int rotate_calls = 0;
    const PipelineFrame output = RunPostPreparationBranch(
        cached,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_ROTATE90,
        decision,
        frame,
        rotator,
        rotate_calls);

    ASSERT_TRUE(output.used_rotator);
    EXPECT_EQ(rotate_calls, 1);
    EXPECT_FALSE(decision.copy_full_frame);
    EXPECT_EQ(output.width, kHeight);
    EXPECT_EQ(output.height, kWidth);
    ASSERT_EQ(output.logical_dirty_regions.size(), 1u);
    EXPECT_EQ(output.logical_dirty_regions.front().left, 1);
    EXPECT_EQ(output.logical_dirty_regions.front().top, 1);
    EXPECT_EQ(output.logical_dirty_regions.front().right, 2);
    EXPECT_EQ(output.logical_dirty_regions.front().bottom, 3);

    ASSERT_EQ(output.logical_move_rects.size(), 1u);
    EXPECT_EQ(output.logical_move_rects.front().DestinationRect.left, 2);
    EXPECT_EQ(output.logical_move_rects.front().DestinationRect.top, 1);
    EXPECT_EQ(output.logical_move_rects.front().DestinationRect.right, 3);
    EXPECT_EQ(output.logical_move_rects.front().DestinationRect.bottom, 3);
}

TEST(CaptureRotationPipeline, FirstRotatedFrameUsesFullRefreshContractInsteadOfBlackFrame) {
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 3;
    constexpr uint32_t kPitch = kWidth * 4u;

    std::vector<uint8_t> cached(static_cast<size_t>(kPitch) * kHeight, 0);
    const std::vector<uint8_t> mapped = MakePattern(kWidth, kHeight);

    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;

    const CaptureDamageDecision decision = ClassifyCaptureDamage({
        .force_first_frame = true,
        .frame = &frame,
    });
    ASSERT_TRUE(decision.copy_full_frame);

    PrepareCaptureFrame(
        decision,
        MappedFrameView{
            .data = mapped.data(),
            .row_pitch = kPitch,
            .width = kWidth,
            .height = kHeight,
        },
        frame,
        cached,
        kPitch);

    const auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(device.Get()));

    int rotate_calls = 0;
    const PipelineFrame output = RunPostPreparationBranch(
        cached,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_ROTATE90,
        decision,
        frame,
        rotator,
        rotate_calls);

    ASSERT_TRUE(output.used_rotator);
    std::vector<uint8_t> viewer_cache(output.storage.size(), 0);
    viewer_cache = ApplyRotatedIncrementalContract(output, viewer_cache);

    EXPECT_EQ(viewer_cache, output.storage)
        << "first rotated frame should advertise a full-refresh contract instead of keeping the viewer black";
}

TEST(CaptureRotationPipeline, RotatedPointerOnlyRefreshRestoresContentInsteadOfMouseTrail) {
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 3;
    constexpr uint32_t kPitch = kWidth * 4u;

    std::vector<uint8_t> cached = MakePattern(kWidth, kHeight);
    const std::vector<uint8_t> mapped = MakePattern(kWidth, kHeight);

    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.pointer_visible = true;
    frame.pointer_x = 1;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 1;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 4;
    frame.pointer_shape_buffer = {0xD0, 0xA0, 0x10, 0xFF};

    const auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    FrameRotator rotator;
    ASSERT_TRUE(rotator.Initialize(device.Get()));

    CaptureDamageDecision initial_decision;
    initial_decision.copy_full_frame = true;
    initial_decision.overlay_pointer = true;
    initial_decision.should_send = true;

    PrepareCaptureFrame(
        initial_decision,
        MappedFrameView{
            .data = mapped.data(),
            .row_pitch = kPitch,
            .width = kWidth,
            .height = kHeight,
        },
        frame,
        cached,
        kPitch);

    int rotate_calls = 0;
    const PipelineFrame initial_output = RunPostPreparationBranch(
        cached,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_ROTATE90,
        initial_decision,
        frame,
        rotator,
        rotate_calls);
    ASSERT_TRUE(initial_output.logical_pointer_region.has_value());
    std::vector<uint8_t> viewer_cache = initial_output.storage;

    PointerSnapshot previous_pointer;
    previous_pointer.visible = true;
    previous_pointer.region = CopyRegion{1, 1, 2, 2};
    previous_pointer.shape_info = frame.pointer_shape_info;

    frame.pointer_x = 2;
    frame.pointer_y = 1;

    const CaptureDamageDecision pointer_decision = ClassifyCaptureDamage({
        .frame = &frame,
        .previous_pointer = previous_pointer,
    });
    ASSERT_EQ(pointer_decision.kind, CaptureUpdateKind::PointerPriorityUpdate);
    ASSERT_TRUE(pointer_decision.copy_previous_pointer_region);
    ASSERT_TRUE(pointer_decision.copy_current_pointer_region);

    PrepareCaptureFrame(
        pointer_decision,
        MappedFrameView{
            .data = mapped.data(),
            .row_pitch = kPitch,
            .width = kWidth,
            .height = kHeight,
        },
        frame,
        cached,
        kPitch);

    const PipelineFrame pointer_output = RunPostPreparationBranch(
        cached,
        kWidth,
        kHeight,
        kPitch,
        DXGI_MODE_ROTATION_ROTATE90,
        pointer_decision,
        frame,
        rotator,
        rotate_calls);

    viewer_cache = ApplyRotatedIncrementalContract(pointer_output, viewer_cache);

    const POINT old_pointer = RotatePointerDrawOrigin(
        POINT{1, 1},
        frame.pointer_shape_info.HotSpot,
        frame.pointer_shape_info.Width,
        frame.pointer_shape_info.Height,
        kWidth,
        kHeight,
        DXGI_MODE_ROTATION_ROTATE90);
    const POINT new_pointer = RotatePointerDrawOrigin(
        POINT{frame.pointer_x, frame.pointer_y},
        frame.pointer_shape_info.HotSpot,
        frame.pointer_shape_info.Width,
        frame.pointer_shape_info.Height,
        kWidth,
        kHeight,
        DXGI_MODE_ROTATION_ROTATE90);

    EXPECT_EQ(PixelAt(viewer_cache.data(), pointer_output.pitch, old_pointer.x, old_pointer.y),
              PixelAt(pointer_output.data, pointer_output.pitch, old_pointer.x, old_pointer.y))
        << "rotated pointer-only refresh should restore the old cursor region instead of leaving a mouse trail";
    EXPECT_EQ(PixelAt(viewer_cache.data(), pointer_output.pitch, new_pointer.x, new_pointer.y),
              PixelAt(pointer_output.data, pointer_output.pitch, new_pointer.x, new_pointer.y));
}

} // namespace
