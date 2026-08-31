#include "host/input_injector.h"

#include <array>
#include <gtest/gtest.h>

using remotex::host::InputInjector;
using remotex::host::InputInjectorTestAccess;
using remotex::host::DesktopContextSnapshot;

namespace {

struct AbsolutePoint {
    LONG x = 0;
    LONG y = 0;
};

DesktopContextSnapshot g_test_desktop_context;

DesktopContextSnapshot ProvideTestDesktopContext()
{
    return g_test_desktop_context;
}

std::vector<uint8_t> BuildSingleKeyDownPayload(uint16_t vkey_code)
{
    std::vector<uint8_t> payload;
    remotex::core::KeyEvent event;
    event.type = remotex::core::InputEventType::KeyDown;
    event.vkey_code = vkey_code;
    event.modifiers = 0;
    remotex::core::SerializeKey(event, payload);
    return payload;
}

class FakeSecureDesktopBridge final : public InputInjector::SecureDesktopBridge {
public:
    bool handled = true;
    bool called = false;
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
    size_t payload_size = 0;
    InputInjector::ProcessInputResult returned_result{};

    bool Submit(std::span<const uint8_t> data,
                LONG monitor_left,
                LONG monitor_top,
                LONG monitor_right,
                LONG monitor_bottom,
                InputInjector::ProcessInputResult& result) override
    {
        called = true;
        left = monitor_left;
        top = monitor_top;
        right = monitor_right;
        bottom = monitor_bottom;
        payload_size = data.size();
        result = returned_result;
        return handled;
    }
};

AbsolutePoint ExpectedDesktopPoint(float normalized_x,
                                   float normalized_y,
                                   LONG monitor_left,
                                   LONG monitor_top,
                                   LONG monitor_right,
                                   LONG monitor_bottom,
                                   int desktop_x,
                                   int desktop_y,
                                   int desktop_width,
                                   int desktop_height)
{
    return {
        InputInjector::MapNormalizedCoordinateToAbsolute(
            normalized_x,
            monitor_left,
            monitor_right - monitor_left,
            desktop_x,
            desktop_width),
        InputInjector::MapNormalizedCoordinateToAbsolute(
            normalized_y,
            monitor_top,
            monitor_bottom - monitor_top,
            desktop_y,
            desktop_height),
    };
}

AbsolutePoint MapThroughInjector(float normalized_x,
                                 float normalized_y,
                                 LONG monitor_left,
                                 LONG monitor_top,
                                 LONG monitor_right,
                                 LONG monitor_bottom,
                                 int desktop_x,
                                 int desktop_y,
                                 int desktop_width,
                                 int desktop_height)
{
    InputInjector injector;
    injector.SetCapturedMonitorBounds(
        monitor_left,
        monitor_top,
        monitor_right,
        monitor_bottom);
    InputInjectorTestAccess::SetVirtualDesktopBounds(
        injector,
        desktop_x,
        desktop_y,
        desktop_width,
        desktop_height);

    LONG abs_x = 0;
    LONG abs_y = 0;
    InputInjectorTestAccess::NormalizedToAbsolute(
        injector,
        normalized_x,
        normalized_y,
        abs_x,
        abs_y);
    return {abs_x, abs_y};
}

void ExpectDesktopCoordinateContract(const char* scenario,
                                     LONG monitor_left,
                                     LONG monitor_top,
                                     LONG monitor_right,
                                     LONG monitor_bottom,
                                     int desktop_x,
                                     int desktop_y,
                                     int desktop_width,
                                     int desktop_height)
{
    struct PointCase {
        float normalized_x;
        float normalized_y;
        const char* label;
    };

    constexpr std::array<PointCase, 5> kPoints{{
        {0.0f, 0.0f, "logical top-left"},
        {1.0f, 0.0f, "logical top-right"},
        {0.0f, 1.0f, "logical bottom-left"},
        {1.0f, 1.0f, "logical bottom-right"},
        {0.5f, 0.5f, "logical center"},
    }};

    for (const auto& point : kPoints) {
        SCOPED_TRACE(scenario);
        SCOPED_TRACE(point.label);
        const AbsolutePoint expected = ExpectedDesktopPoint(
            point.normalized_x,
            point.normalized_y,
            monitor_left,
            monitor_top,
            monitor_right,
            monitor_bottom,
            desktop_x,
            desktop_y,
            desktop_width,
            desktop_height);
        const AbsolutePoint actual = MapThroughInjector(
            point.normalized_x,
            point.normalized_y,
            monitor_left,
            monitor_top,
            monitor_right,
            monitor_bottom,
            desktop_x,
            desktop_y,
            desktop_width,
            desktop_height);

        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
    }
}

} // namespace

TEST(InputInjector, MapsFullDesktopExtentsToAbsoluteRange) {
    EXPECT_EQ(InputInjector::MapNormalizedCoordinateToAbsolute(0.0f, 0, 1920, 0, 1920), 0);
    EXPECT_EQ(InputInjector::MapNormalizedCoordinateToAbsolute(1.0f, 0, 1920, 0, 1920), 65535);
    EXPECT_EQ(InputInjector::MapNormalizedCoordinateToAbsolute(1.0f, 0, 1080, 0, 1080), 65535);
}

TEST(InputInjector, MapsRotated90LogicalCornersIntoDesktopCoordinates) {
    ExpectDesktopCoordinateContract(
        "rotated 90 monitor",
        100,
        200,
        1180,
        2120,
        0,
        0,
        3000,
        2200);
}

TEST(InputInjector, MapsRotated270LogicalCornersIntoDesktopCoordinates) {
    ExpectDesktopCoordinateContract(
        "rotated 270 monitor",
        1600,
        80,
        2680,
        2000,
        0,
        0,
        3200,
        2160);
}

TEST(InputInjector, MapsOffsetPortraitMonitorIntoVirtualDesktopCoordinates) {
    ExpectDesktopCoordinateContract(
        "offset portrait secondary monitor",
        -1080,
        240,
        0,
        2160,
        -1080,
        0,
        3000,
        2160);
}

TEST(InputInjector, ClampsOutOfRangeCoordinatesBeforeDesktopSpaceMapping) {
    const AbsolutePoint expected = ExpectedDesktopPoint(
        -1.0f,
        2.0f,
        -1080,
        240,
        0,
        2160,
        -1080,
        0,
        3000,
        2160);
    const AbsolutePoint actual = MapThroughInjector(
        -1.0f,
        2.0f,
        -1080,
        240,
        0,
        2160,
        -1080,
        0,
        3000,
        2160);

    EXPECT_EQ(actual.x, expected.x);
    EXPECT_EQ(actual.y, expected.y);
}

TEST(InputInjector, RoutesSecureDesktopInputThroughBridgeWhenAvailable) {
    InputInjector injector;
    injector.Start();
    injector.SetCapturedMonitorBounds(100, 200, 1180, 2120);
    InputInjectorTestAccess::SetDesktopContextProvider(injector, &ProvideTestDesktopContext);

    g_test_desktop_context = {};
    g_test_desktop_context.input_desktop_name = "Winlogon";
    g_test_desktop_context.thread_desktop_name = "default";
    g_test_desktop_context.input_desktop_available = true;
    g_test_desktop_context.thread_desktop_available = true;
    g_test_desktop_context.input_error = ERROR_SUCCESS;
    g_test_desktop_context.thread_error = ERROR_SUCCESS;

    FakeSecureDesktopBridge bridge;
    bridge.returned_result.events_processed = 1;
    bridge.returned_result.input_desktop_name = "Winlogon";
    bridge.returned_result.thread_desktop_name = "helper";
    injector.SetSecureDesktopBridge(&bridge);

    const auto payload = BuildSingleKeyDownPayload('A');
    const auto result = injector.ProcessInputData(payload);

    EXPECT_TRUE(bridge.called);
    EXPECT_EQ(bridge.left, 100);
    EXPECT_EQ(bridge.top, 200);
    EXPECT_EQ(bridge.right, 1180);
    EXPECT_EQ(bridge.bottom, 2120);
    EXPECT_EQ(bridge.payload_size, payload.size());
    EXPECT_TRUE(result.secure_desktop_bridge_attempted);
    EXPECT_TRUE(result.secure_desktop_bridge_used);
    EXPECT_EQ(result.events_processed, 1u);
    EXPECT_EQ(result.input_desktop_name, "Winlogon");
    EXPECT_EQ(result.thread_desktop_name, "helper");
    EXPECT_EQ(result.send_input_failures, 0u);
}

TEST(InputInjector, DoesNotRouteDefaultDesktopInputThroughSecureDesktopBridge) {
    InputInjector injector;
    injector.Start();
    InputInjectorTestAccess::SetDesktopContextProvider(injector, &ProvideTestDesktopContext);

    g_test_desktop_context = {};
    g_test_desktop_context.input_desktop_name = "default";
    g_test_desktop_context.thread_desktop_name = "default";
    g_test_desktop_context.input_desktop_available = true;
    g_test_desktop_context.thread_desktop_available = true;
    g_test_desktop_context.input_error = ERROR_SUCCESS;
    g_test_desktop_context.thread_error = ERROR_SUCCESS;

    FakeSecureDesktopBridge bridge;
    injector.SetSecureDesktopBridge(&bridge);

    const std::span<const uint8_t> empty_payload;
    const auto result = injector.ProcessInputData(empty_payload);

    EXPECT_FALSE(bridge.called);
    EXPECT_FALSE(result.secure_desktop_bridge_attempted);
    EXPECT_FALSE(result.secure_desktop_bridge_used);
    EXPECT_EQ(result.events_processed, 0u);
}

TEST(InputInjector, DeterminesNumLockAlignmentWhenAlreadyAligned) {
    EXPECT_EQ(
        InputInjector::DetermineNumLockAlignmentAction(true, true),
        InputInjector::NumLockAlignmentAction::NoOp);
    EXPECT_EQ(
        InputInjector::DetermineNumLockAlignmentAction(false, false),
        InputInjector::NumLockAlignmentAction::NoOp);
}

TEST(InputInjector, DeterminesNumLockAlignmentWhenToggleIsRequired) {
    EXPECT_EQ(
        InputInjector::DetermineNumLockAlignmentAction(true, false),
        InputInjector::NumLockAlignmentAction::ToggleOnce);
    EXPECT_EQ(
        InputInjector::DetermineNumLockAlignmentAction(false, true),
        InputInjector::NumLockAlignmentAction::ToggleOnce);
}

TEST(InputInjector, RefusesToGuessNumLockAlignmentWithoutReadableState) {
    EXPECT_EQ(
        InputInjector::DetermineNumLockAlignmentAction(std::nullopt, true),
        InputInjector::NumLockAlignmentAction::CannotDetermine);
    EXPECT_EQ(
        InputInjector::DetermineNumLockAlignmentAction(std::nullopt, false),
        InputInjector::NumLockAlignmentAction::CannotDetermine);
}

TEST(InputInjector, TreatsNumLockAsExtendedKey) {
    EXPECT_TRUE(InputInjector::IsExtendedKey(VK_NUMLOCK));
    EXPECT_TRUE(InputInjector::IsExtendedKey(VK_DIVIDE));
    EXPECT_FALSE(InputInjector::IsExtendedKey('A'));
}

TEST(InputInjector, AlignmentResultHelpersExposeNoOpAndToggleStates) {
    InputInjector::NumLockAlignmentResult already_aligned{};
    already_aligned.action = InputInjector::NumLockAlignmentAction::NoOp;
    already_aligned.current_state = true;
    already_aligned.requested_state = true;
    already_aligned.applied = true;

    EXPECT_TRUE(already_aligned.already_aligned());
    EXPECT_FALSE(already_aligned.needs_toggle());

    InputInjector::NumLockAlignmentResult toggled{};
    toggled.action = InputInjector::NumLockAlignmentAction::ToggleOnce;
    toggled.current_state = false;
    toggled.requested_state = true;
    toggled.applied = true;

    EXPECT_FALSE(toggled.already_aligned());
    EXPECT_TRUE(toggled.needs_toggle());

    InputInjector::NumLockAlignmentResult failed{};
    failed.action = InputInjector::NumLockAlignmentAction::CannotDetermine;
    failed.current_state = std::nullopt;
    failed.requested_state = true;
    failed.applied = false;
    failed.windows_error = ERROR_INVALID_STATE;

    EXPECT_FALSE(failed.already_aligned());
    EXPECT_FALSE(failed.needs_toggle());
    EXPECT_EQ(failed.windows_error, ERROR_INVALID_STATE);
}
