#include <gtest/gtest.h>

#include "host/capture_damage.h"
#include "host/capture_frame_preparer.h"

#include <array>
#include <vector>

namespace {

using remotex::host::CaptureDamageContext;
using remotex::host::CaptureUpdateKind;
using remotex::host::CapturedFrame;
using remotex::host::ClassifyCaptureDamage;
using remotex::host::CopyRegion;
using remotex::host::DirtyRect;
using remotex::host::FramePreparationResult;
using remotex::host::MappedFrameView;
using remotex::host::PointerSnapshot;
using remotex::host::PrepareCaptureFrame;

constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 3;
constexpr uint32_t kPitch = kWidth * 4;

std::vector<uint8_t> MakePatternBuffer() {
    std::vector<uint8_t> buffer(static_cast<size_t>(kPitch) * kHeight, 0);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t offset = static_cast<size_t>(y) * kPitch + static_cast<size_t>(x) * 4u;
            buffer[offset + 0] = static_cast<uint8_t>(10 * y + x);
            buffer[offset + 1] = static_cast<uint8_t>(20 * y + x);
            buffer[offset + 2] = static_cast<uint8_t>(30 * y + x);
            buffer[offset + 3] = 255;
        }
    }
    return buffer;
}

CapturedFrame MakeBaseFrame() {
    CapturedFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    return frame;
}

TEST(CaptureDamage, MoveRectsUsePartialUpdateInsteadOfFullRefresh) {
    CapturedFrame frame = MakeBaseFrame();
    frame.move_rects.push_back(DXGI_OUTDUPL_MOVE_RECT{
        .SourcePoint = POINT{0, 0},
        .DestinationRect = RECT{1, 0, 3, 1},
    });

    const auto decision = ClassifyCaptureDamage({
        .frame = &frame,
    });

    EXPECT_EQ(decision.kind, CaptureUpdateKind::PartialUpdate);
    EXPECT_FALSE(decision.copy_full_frame);
    EXPECT_TRUE(decision.replay_move_rects);
    EXPECT_TRUE(decision.should_send);
}

TEST(CaptureDamage, PointerOnlyFramesUsePointerPriorityUpdate) {
    CapturedFrame frame = MakeBaseFrame();
    frame.pointer_visible = true;
    frame.pointer_x = 2;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 1;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 4;
    frame.pointer_shape_buffer = {0x44, 0x55, 0x66, 0xFF};

    PointerSnapshot previous_pointer;
    previous_pointer.visible = true;
    previous_pointer.region = CopyRegion{0, 0, 1, 1};
    previous_pointer.shape_info = frame.pointer_shape_info;

    const auto decision = ClassifyCaptureDamage({
        .frame = &frame,
        .previous_pointer = previous_pointer,
    });

    EXPECT_EQ(decision.kind, CaptureUpdateKind::PointerPriorityUpdate);
    EXPECT_TRUE(decision.pointer_changed);
    EXPECT_TRUE(decision.copy_previous_pointer_region);
    EXPECT_TRUE(decision.copy_current_pointer_region);
    EXPECT_TRUE(decision.overlay_pointer);
    EXPECT_TRUE(decision.should_send);
}

TEST(CaptureDamage, StructuralChangesForceFullRefresh) {
    CapturedFrame frame = MakeBaseFrame();

    const auto decision = ClassifyCaptureDamage({
        .structural_change = true,
        .frame = &frame,
    });

    EXPECT_EQ(decision.kind, CaptureUpdateKind::FullRefresh);
    EXPECT_TRUE(decision.copy_full_frame);
    EXPECT_TRUE(decision.should_send);
}

TEST(CaptureDamage, FirstFrameBootstrapForcesFullRefresh) {
    CapturedFrame frame = MakeBaseFrame();
    frame.dirty_rects.push_back(DirtyRect{0, 0, 1, 1});

    const auto decision = ClassifyCaptureDamage({
        .force_first_frame = true,
        .frame = &frame,
    });

    EXPECT_EQ(decision.kind, CaptureUpdateKind::FullRefresh);
    EXPECT_TRUE(decision.force_first_frame);
    EXPECT_TRUE(decision.copy_full_frame);
}

TEST(CaptureDamage, MissingMetadataForcesFullRefresh) {
    CapturedFrame frame = MakeBaseFrame();

    const auto decision = ClassifyCaptureDamage({
        .frame = &frame,
    });

    EXPECT_EQ(decision.kind, CaptureUpdateKind::FullRefresh);
    EXPECT_TRUE(decision.missing_damage_metadata);
    EXPECT_TRUE(decision.copy_full_frame);
}

TEST(CaptureDamage, BufferResizeForcesFullRefreshEvenWithDirtyRects) {
    CapturedFrame frame = MakeBaseFrame();
    frame.dirty_rects.push_back(DirtyRect{1, 1, 2, 1});

    const auto decision = ClassifyCaptureDamage({
        .buffer_resized = true,
        .frame = &frame,
    });

    EXPECT_EQ(decision.kind, CaptureUpdateKind::FullRefresh);
    EXPECT_TRUE(decision.buffer_resized);
    EXPECT_TRUE(decision.copy_full_frame);
}

TEST(CaptureFramePreparer, ReplaysMoveRectsBeforeDirtyCopies) {
    std::vector<uint8_t> cached = MakePatternBuffer();
    std::vector<uint8_t> mapped = MakePatternBuffer();
    const size_t dirty_offset = static_cast<size_t>(1) * kPitch + static_cast<size_t>(2) * 4u;
    mapped[dirty_offset + 0] = 201;
    mapped[dirty_offset + 1] = 202;
    mapped[dirty_offset + 2] = 203;

    CapturedFrame frame = MakeBaseFrame();
    frame.move_rects.push_back(DXGI_OUTDUPL_MOVE_RECT{
        .SourcePoint = POINT{0, 0},
        .DestinationRect = RECT{1, 0, 3, 1},
    });
    frame.dirty_rects.push_back(DirtyRect{2, 1, 1, 1});

    const auto decision = ClassifyCaptureDamage({
        .frame = &frame,
    });

    FramePreparationResult result = PrepareCaptureFrame(
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

    EXPECT_GT(result.copied_bytes, 0u);

    const size_t moved_dst = static_cast<size_t>(0) * kPitch + static_cast<size_t>(1) * 4u;
    EXPECT_EQ(cached[moved_dst + 0], 0u);
    EXPECT_EQ(cached[moved_dst + 1], 0u);
    EXPECT_EQ(cached[moved_dst + 2], 0u);

    EXPECT_EQ(cached[dirty_offset + 0], 201u);
    EXPECT_EQ(cached[dirty_offset + 1], 202u);
    EXPECT_EQ(cached[dirty_offset + 2], 203u);
}

TEST(CaptureFramePreparer, PointerPriorityInvalidatesOldRegionAndOverlaysNewPointer) {
    std::vector<uint8_t> cached(static_cast<size_t>(kPitch) * kHeight, 0);
    std::vector<uint8_t> mapped = MakePatternBuffer();

    CapturedFrame frame = MakeBaseFrame();
    frame.pointer_visible = true;
    frame.pointer_x = 2;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 1;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 4;
    frame.pointer_shape_buffer = {0x99, 0x88, 0x77, 0xFF};

    PointerSnapshot previous_pointer;
    previous_pointer.visible = true;
    previous_pointer.region = CopyRegion{0, 0, 1, 1};
    previous_pointer.shape_info = frame.pointer_shape_info;

    const auto decision = ClassifyCaptureDamage({
        .frame = &frame,
        .previous_pointer = previous_pointer,
    });

    const FramePreparationResult result = PrepareCaptureFrame(
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

    EXPECT_EQ(decision.kind, CaptureUpdateKind::PointerPriorityUpdate);
    EXPECT_TRUE(result.pointer_overlay_applied);

    EXPECT_EQ(cached[0], mapped[0]);
    EXPECT_EQ(cached[1], mapped[1]);
    EXPECT_EQ(cached[2], mapped[2]);

    const size_t pointer_offset = static_cast<size_t>(1) * kPitch + static_cast<size_t>(2) * 4u;
    EXPECT_EQ(cached[pointer_offset + 0], 0x99);
    EXPECT_EQ(cached[pointer_offset + 1], 0x88);
    EXPECT_EQ(cached[pointer_offset + 2], 0x77);
    EXPECT_EQ(cached[pointer_offset + 3], 0xFF);
}

TEST(CaptureFramePreparer, PointerPriorityContractCopiesPointerRegionsBeforeRotatedRefresh) {
    std::vector<uint8_t> cached(static_cast<size_t>(kPitch) * kHeight, 0);
    std::vector<uint8_t> mapped = MakePatternBuffer();

    CapturedFrame frame = MakeBaseFrame();
    frame.pointer_visible = true;
    frame.pointer_x = 3;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 1;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 4;
    frame.pointer_shape_buffer = {0xAA, 0xBB, 0xCC, 0xFF};

    PointerSnapshot previous_pointer;
    previous_pointer.visible = true;
    previous_pointer.region = CopyRegion{1, 1, 2, 2};
    previous_pointer.shape_info = frame.pointer_shape_info;

    const auto decision = ClassifyCaptureDamage({
        .frame = &frame,
        .previous_pointer = previous_pointer,
    });

    ASSERT_EQ(decision.kind, CaptureUpdateKind::PointerPriorityUpdate);
    ASSERT_TRUE(decision.copy_previous_pointer_region);
    ASSERT_TRUE(decision.copy_current_pointer_region);
    ASSERT_FALSE(decision.copy_dirty_regions);

    const FramePreparationResult result = PrepareCaptureFrame(
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

    EXPECT_TRUE(result.pointer_overlay_applied);
    EXPECT_EQ(result.copied_bytes, 8u)
        << "pointer-priority refresh should explicitly copy both pointer-invalidated source pixels";

    const size_t old_pointer_offset = static_cast<size_t>(1) * kPitch + static_cast<size_t>(1) * 4u;
    EXPECT_EQ(cached[old_pointer_offset + 0], mapped[old_pointer_offset + 0]);
    EXPECT_EQ(cached[old_pointer_offset + 1], mapped[old_pointer_offset + 1]);
    EXPECT_EQ(cached[old_pointer_offset + 2], mapped[old_pointer_offset + 2]);

    const size_t new_pointer_offset = static_cast<size_t>(1) * kPitch + static_cast<size_t>(3) * 4u;
    EXPECT_EQ(cached[new_pointer_offset + 0], 0xAA);
    EXPECT_EQ(cached[new_pointer_offset + 1], 0xBB);
    EXPECT_EQ(cached[new_pointer_offset + 2], 0xCC);
    EXPECT_EQ(cached[new_pointer_offset + 3], 0xFF);
}

} // namespace
