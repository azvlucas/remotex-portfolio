#include <gtest/gtest.h>

#include "host/capture_damage.h"
#include "host/rotation_geometry.h"

namespace {

using remotex::host::CopyRegion;
using remotex::host::RotateMoveRect;
using remotex::host::RotatePoint;
using remotex::host::RotatePointerDrawOrigin;
using remotex::host::RotateRegion;
using remotex::host::MapPointerDrawOriginIntoDesktopRegion;

TEST(RotationGeometry, RotateRegion90SwapsAxes) {
    const CopyRegion source{100, 50, 140, 90};

    const CopyRegion rotated_90 = RotateRegion(source, 1920, 1080, DXGI_MODE_ROTATION_ROTATE90);
    EXPECT_EQ(rotated_90.left, 990);
    EXPECT_EQ(rotated_90.top, 100);
    EXPECT_EQ(rotated_90.right, 1030);
    EXPECT_EQ(rotated_90.bottom, 140);

    const CopyRegion rotated_270 = RotateRegion(source, 1920, 1080, DXGI_MODE_ROTATION_ROTATE270);
    EXPECT_EQ(rotated_270.left, 50);
    EXPECT_EQ(rotated_270.top, 1780);
    EXPECT_EQ(rotated_270.right, 90);
    EXPECT_EQ(rotated_270.bottom, 1820);
}

TEST(RotationGeometry, RotateMoveRectPreservesExtent) {
    const DXGI_OUTDUPL_MOVE_RECT source{
        .SourcePoint = POINT{20, 30},
        .DestinationRect = RECT{100, 50, 160, 90},
    };

    const DXGI_OUTDUPL_MOVE_RECT rotated =
        RotateMoveRect(source, 1920, 1080, DXGI_MODE_ROTATION_ROTATE90);

    EXPECT_EQ(rotated.DestinationRect.left, 990);
    EXPECT_EQ(rotated.DestinationRect.top, 100);
    EXPECT_EQ(rotated.DestinationRect.right, 1030);
    EXPECT_EQ(rotated.DestinationRect.bottom, 160);
    EXPECT_EQ(rotated.DestinationRect.right - rotated.DestinationRect.left,
              source.DestinationRect.bottom - source.DestinationRect.top);
    EXPECT_EQ(rotated.DestinationRect.bottom - rotated.DestinationRect.top,
              source.DestinationRect.right - source.DestinationRect.left);
    EXPECT_EQ(rotated.SourcePoint.x, 1049);
    EXPECT_EQ(rotated.SourcePoint.y, 20);
}

TEST(RotationGeometry, RotatePointMatchesLogicalCoordinates) {
    const POINT source{25, 10};

    const POINT identity = RotatePoint(source, 100, 80, DXGI_MODE_ROTATION_IDENTITY);
    EXPECT_EQ(identity.x, 25);
    EXPECT_EQ(identity.y, 10);

    const POINT rotated_90 = RotatePoint(source, 100, 80, DXGI_MODE_ROTATION_ROTATE90);
    EXPECT_EQ(rotated_90.x, 69);
    EXPECT_EQ(rotated_90.y, 25);

    const POINT rotated_180 = RotatePoint(source, 100, 80, DXGI_MODE_ROTATION_ROTATE180);
    EXPECT_EQ(rotated_180.x, 74);
    EXPECT_EQ(rotated_180.y, 69);

    const POINT rotated_270 = RotatePoint(source, 100, 80, DXGI_MODE_ROTATION_ROTATE270);
    EXPECT_EQ(rotated_270.x, 10);
    EXPECT_EQ(rotated_270.y, 74);
}

TEST(RotationGeometry, RotatePointerDrawOriginAccountsForHotspotAndExtent) {
    const POINT pointer_position{8, 4};
    const POINT rotated_hotspot = RotatePoint(pointer_position,
                                              16,
                                              10,
                                              DXGI_MODE_ROTATION_ROTATE90);
    const POINT draw_origin = RotatePointerDrawOrigin(pointer_position,
                                                      POINT{1, 2},
                                                      3,
                                                      4,
                                                      16,
                                                      10,
                                                      DXGI_MODE_ROTATION_ROTATE90);

    EXPECT_EQ(draw_origin.x, 4);
    EXPECT_EQ(draw_origin.y, 7);
    EXPECT_NE(draw_origin.x, rotated_hotspot.x);
    EXPECT_NE(draw_origin.y, rotated_hotspot.y);
}

TEST(RotationGeometry, MapPointerDrawOriginIntoDesktopRegionUsesLogicalDesktopCoordinates) {
    const RECT desktop_region{100, 200, 1180, 2120};
    const POINT draw_origin = MapPointerDrawOriginIntoDesktopRegion(
        POINT{350, 640},
        POINT{4, 6},
        desktop_region);

    EXPECT_EQ(draw_origin.x, 246);
    EXPECT_EQ(draw_origin.y, 434);
}

} // namespace
