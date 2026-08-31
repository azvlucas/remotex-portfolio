#include <gtest/gtest.h>

#include "viewer/remote_session_window.h"
#include "viewer/video_renderer.h"

namespace {

using remotex::viewer::ParseMonitorChangedContract;
using remotex::viewer::RendererFrameState;
using remotex::viewer::RequiresRendererResetForDisplayChange;
using remotex::viewer::SessionDisplayContract;
using remotex::viewer::ShouldResetRendererForDisplayContractUpdate;

TEST(ViewerRotationSessionContract, MonitorChangedPreservesRotationAwareDisplayMetadata) {
    const auto parsed = ParseMonitorChangedContract(
        R"({"type":"monitor_changed","adapter_index":2,"output_index":1,"width":1080,"height":1920,"rotation":1})");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->adapter_index, 2);
    EXPECT_EQ(parsed->output_index, 1);
    EXPECT_EQ(parsed->width, 1080);
    EXPECT_EQ(parsed->height, 1920);
    EXPECT_EQ(parsed->rotation, 1);
}

TEST(ViewerRotationSessionContract, ResetRequiredWhenGeometryOrRotationChangesButNotForIdenticalContract) {
    const SessionDisplayContract current{
        .adapter_index = 0,
        .output_index = 0,
        .width = 1920,
        .height = 1080,
        .rotation = 0,
    };
    const RendererFrameState idle_state{};

    EXPECT_FALSE(RequiresRendererResetForDisplayChange(current, current, idle_state));

    EXPECT_TRUE(RequiresRendererResetForDisplayChange(
        current,
        SessionDisplayContract{
            .adapter_index = 0,
            .output_index = 0,
            .width = 1080,
            .height = 1920,
            .rotation = 1,
        },
        idle_state));

    EXPECT_TRUE(RequiresRendererResetForDisplayChange(
        current,
        SessionDisplayContract{
            .adapter_index = 0,
            .output_index = 0,
            .width = 1920,
            .height = 1080,
            .rotation = 2,
        },
        idle_state));
}

TEST(ViewerRotationSessionContract, InitialRotatedContractDoesNotForceViewerResetWhenHostOwnsBootstrap) {
    const SessionDisplayContract rotated{
        .adapter_index = 0,
        .output_index = 0,
        .width = 1080,
        .height = 1920,
        .rotation = 1,
    };
    const RendererFrameState stale_state{
        .frame_width = 1080,
        .frame_height = 1920,
        .has_pending_frame = false,
        .has_displayed_frame = true,
        .textures_created = true,
        .texture_width = 1080,
        .texture_height = 1920,
    };

    EXPECT_FALSE(ShouldResetRendererForDisplayContractUpdate(std::nullopt, stale_state, rotated));
}

TEST(ViewerRotationSessionContract, ResetRequiredWhenStaleRendererStateWouldSurviveRotatedBootstrap) {
    const SessionDisplayContract current{
        .adapter_index = 0,
        .output_index = 0,
        .width = 1920,
        .height = 1080,
        .rotation = 0,
    };
    const SessionDisplayContract rotated{
        .adapter_index = 0,
        .output_index = 0,
        .width = 1080,
        .height = 1920,
        .rotation = 1,
    };

    const RendererFrameState stale_state{
        .frame_width = 1920,
        .frame_height = 1080,
        .has_pending_frame = false,
        .has_displayed_frame = true,
        .textures_created = true,
        .texture_width = 1920,
        .texture_height = 1080,
    };

    EXPECT_TRUE(RequiresRendererResetForDisplayChange(current, rotated, stale_state));
}

} // namespace
