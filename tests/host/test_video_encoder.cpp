#include <gtest/gtest.h>

#include "host/video_encoder.h"

#include <vector>

namespace remotex::host {
namespace {

FrameSlotData MakeSlotData(uint64_t id, uint32_t width = 64, uint32_t height = 64) {
    FrameSlotData data;
    data.frame_id = id;
    data.width = width;
    data.height = height;
    data.pitch = width * 4;
    data.bgra_data.resize(static_cast<size_t>(data.pitch) * height, 0xAA);
    data.metrics.frame_id = id;
    return data;
}

} // namespace

TEST(BoundedFrameSlot, EnqueueIntoEmpty_NoDrop) {
    BoundedFrameSlot slot;

    EXPECT_FALSE(slot.TryEnqueue(MakeSlotData(7)));

    const auto dequeued = slot.TryDequeue();
    ASSERT_TRUE(dequeued.has_value());
    EXPECT_EQ(dequeued->frame_id, 7u);
    EXPECT_EQ(slot.GetDropCount(), 0u);
}

TEST(BoundedFrameSlot, EnqueueIntoOccupied_DropsOld) {
    BoundedFrameSlot slot;

    EXPECT_FALSE(slot.TryEnqueue(MakeSlotData(1)));
    EXPECT_TRUE(slot.TryEnqueue(MakeSlotData(2)));

    const auto dequeued = slot.TryDequeue();
    ASSERT_TRUE(dequeued.has_value());
    EXPECT_EQ(dequeued->frame_id, 2u);
    EXPECT_EQ(slot.GetDropCount(), 1u);
}

TEST(BoundedFrameSlot, DequeueEmpty_ReturnsNullopt) {
    BoundedFrameSlot slot;

    EXPECT_FALSE(slot.TryDequeue().has_value());
}

TEST(BoundedFrameSlot, ResetDropCount_ReturnsAndClears) {
    BoundedFrameSlot slot;

    EXPECT_FALSE(slot.TryEnqueue(MakeSlotData(10)));
    EXPECT_TRUE(slot.TryEnqueue(MakeSlotData(11)));
    EXPECT_EQ(slot.ResetDropCount(), 1u);
    EXPECT_EQ(slot.GetDropCount(), 0u);
}

} // namespace remotex::host
