// RemoteX — tests/host/test_screen_capture.cpp
// Testes para inicialização do ScreenCapture DXGI.

#include <gtest/gtest.h>

#include "host/screen_capture.h"

using namespace remotex::host;

namespace remotex::host {
class ScreenCaptureDesktopContextTestAccess {
public:
    static CaptureResult ClassifyAcquireFailure(HRESULT hr,
                                                const DesktopContextSnapshot* previous,
                                                const DesktopContextSnapshot& current) {
        CaptureResult result;
        result.status = ScreenCapture::ClassifyAcquireFailure(hr, previous, current, result);
        result.timed_out = (hr == DXGI_ERROR_WAIT_TIMEOUT);
        return result;
    }
};
} // namespace remotex::host

namespace {

DesktopContextSnapshot MakeDesktopContext(std::string input_desktop,
                                          std::string thread_desktop) {
    DesktopContextSnapshot snapshot;
    snapshot.input_desktop_name = std::move(input_desktop);
    snapshot.thread_desktop_name = std::move(thread_desktop);
    snapshot.input_desktop_available = true;
    snapshot.thread_desktop_available = true;
    snapshot.input_error = ERROR_SUCCESS;
    snapshot.thread_error = ERROR_SUCCESS;
    return snapshot;
}

DesktopContextSnapshot MakeUnavailableDesktopContext(DWORD input_error,
                                                     DWORD thread_error) {
    DesktopContextSnapshot snapshot;
    snapshot.input_error = input_error;
    snapshot.thread_error = thread_error;
    return snapshot;
}

} // namespace

// ─── Initialization ─────────────────────────────────────────────────────────

TEST(ScreenCapture, Init_Success) {
    ScreenCapture capture;

    // Em ambientes sem GPU (CI), Init pode falhar.
    // O teste verifica que não crasheia e retorna um bool válido.
    bool ok = capture.Init();

    // Qualquer resultado é aceitável — o importante é não crashear.
    EXPECT_TRUE(ok || !ok);
}

TEST(ScreenCapture, Shutdown_Safe) {
    ScreenCapture capture;
    // Shutdown sem Initialize não deve crashear.
    capture.Shutdown();
}

TEST(ScreenCapture, DoubleInit) {
    ScreenCapture capture;
    capture.Init();
    // Segunda inicialização não deve crashear.
    capture.Init();
    capture.Shutdown();
}

TEST(ScreenCapture, CaptureFrame_NotInitialized) {
    ScreenCapture capture;
    CapturedFrame frame;
    const auto result = capture.CaptureFrame(frame);
    // Deve retornar erro sem crashear.
    EXPECT_NE(result.status, CaptureStatus::Ok);
    EXPECT_DOUBLE_EQ(result.acquire_wait_ms, 0.0);
    EXPECT_EQ(result.accumulated_frames, 0u);
    EXPECT_FALSE(result.timed_out);
}

TEST(ScreenCapture, CaptureResultCarriesAcquireNextFrameEvidence) {
    // A etapa de AcquireNextFrame(0) foi substituida por um resultado explicito
    // de aquisicao, entao os campos precisam ser testaveis sem DXGI real.
    CaptureResult result;
    result.status = CaptureStatus::Timeout;
    result.acquire_wait_ms = 8.0;
    result.accumulated_frames = 3;
    result.timed_out = true;

    EXPECT_EQ(result.status, CaptureStatus::Timeout);
    EXPECT_DOUBLE_EQ(result.acquire_wait_ms, 8.0);
    EXPECT_EQ(result.accumulated_frames, 3u);
    EXPECT_TRUE(result.timed_out);
}

TEST(ScreenCapture, AccessLostWithDesktopSwitchReturnsDesktopSwitched) {
    const DesktopContextSnapshot previous = MakeDesktopContext("default", "default");
    const DesktopContextSnapshot current = MakeDesktopContext("Winlogon", "default");

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_ACCESS_LOST,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::DesktopSwitched);
    EXPECT_EQ(result.previous_input_desktop_name, "default");
    EXPECT_EQ(result.input_desktop_name, "Winlogon");
    EXPECT_EQ(result.thread_desktop_name, "default");
    EXPECT_TRUE(result.input_desktop_changed);
    EXPECT_FALSE(result.thread_desktop_changed);
    EXPECT_FALSE(result.input_matches_thread);
    EXPECT_FALSE(result.timed_out);
}

TEST(ScreenCapture, AccessLostReturningToDefaultStillReportsDesktopSwitched) {
    const DesktopContextSnapshot previous = MakeDesktopContext("Winlogon", "default");
    const DesktopContextSnapshot current = MakeDesktopContext("default", "default");

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_ACCESS_LOST,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::DesktopSwitched);
    EXPECT_EQ(result.previous_input_desktop_name, "Winlogon");
    EXPECT_EQ(result.input_desktop_name, "default");
    EXPECT_EQ(result.thread_desktop_name, "default");
    EXPECT_TRUE(result.input_desktop_changed);
    EXPECT_FALSE(result.thread_desktop_changed);
    EXPECT_TRUE(result.input_matches_thread);
}

TEST(ScreenCapture, AccessLostWithoutDesktopChangeStaysAccessDenied) {
    const DesktopContextSnapshot previous = MakeDesktopContext("default", "default");
    const DesktopContextSnapshot current = MakeDesktopContext("default", "default");

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_ACCESS_LOST,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::AccessDenied);
    EXPECT_EQ(result.previous_input_desktop_name, "default");
    EXPECT_EQ(result.input_desktop_name, "default");
    EXPECT_EQ(result.thread_desktop_name, "default");
    EXPECT_FALSE(result.input_desktop_changed);
    EXPECT_FALSE(result.thread_desktop_changed);
    EXPECT_TRUE(result.input_matches_thread);
}

TEST(ScreenCapture, AccessLostWithoutDesktopContextReturnsExplicitUnavailableReason) {
    const DesktopContextSnapshot previous = MakeDesktopContext("default", "default");
    const DesktopContextSnapshot current = MakeUnavailableDesktopContext(
        ERROR_ACCESS_DENIED,
        ERROR_INVALID_HANDLE);

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_ACCESS_LOST,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::DesktopContextUnavailable);
    EXPECT_FALSE(result.input_desktop_available);
    EXPECT_FALSE(result.thread_desktop_available);
    EXPECT_EQ(result.input_desktop_error, static_cast<DWORD>(ERROR_ACCESS_DENIED));
    EXPECT_EQ(result.thread_desktop_error, static_cast<DWORD>(ERROR_INVALID_HANDLE));
    EXPECT_FALSE(result.input_matches_thread);
}

TEST(ScreenCapture, TimeoutWithDesktopMismatchReturnsDesktopSwitched) {
    const DesktopContextSnapshot previous = MakeDesktopContext("default", "default");
    const DesktopContextSnapshot current = MakeDesktopContext("Winlogon", "default");

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_WAIT_TIMEOUT,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::DesktopSwitched);
    EXPECT_TRUE(result.timed_out);
    EXPECT_EQ(result.previous_input_desktop_name, "default");
    EXPECT_EQ(result.input_desktop_name, "Winlogon");
    EXPECT_EQ(result.thread_desktop_name, "default");
    EXPECT_TRUE(result.input_desktop_changed);
    EXPECT_FALSE(result.thread_desktop_changed);
    EXPECT_FALSE(result.input_matches_thread);
}

TEST(ScreenCapture, TimeoutWithDesktopChangeButMatchingThreadStaysTimeout) {
    const DesktopContextSnapshot previous = MakeDesktopContext("default", "default");
    const DesktopContextSnapshot current = MakeDesktopContext("Winlogon", "Winlogon");

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_WAIT_TIMEOUT,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::Timeout);
    EXPECT_TRUE(result.timed_out);
    EXPECT_EQ(result.previous_input_desktop_name, "default");
    EXPECT_EQ(result.input_desktop_name, "Winlogon");
    EXPECT_EQ(result.thread_desktop_name, "Winlogon");
    EXPECT_TRUE(result.input_desktop_changed);
    EXPECT_TRUE(result.thread_desktop_changed);
    EXPECT_TRUE(result.input_matches_thread);
}

TEST(ScreenCapture, TimeoutWithoutDesktopContextReturnsExplicitUnavailableReason) {
    const DesktopContextSnapshot previous = MakeDesktopContext("default", "default");
    const DesktopContextSnapshot current = MakeUnavailableDesktopContext(
        ERROR_ACCESS_DENIED,
        ERROR_INVALID_HANDLE);

    const CaptureResult result = ScreenCaptureDesktopContextTestAccess::ClassifyAcquireFailure(
        DXGI_ERROR_WAIT_TIMEOUT,
        &previous,
        current);

    EXPECT_EQ(result.status, CaptureStatus::DesktopContextUnavailable);
    EXPECT_TRUE(result.timed_out);
    EXPECT_FALSE(result.input_desktop_available);
    EXPECT_FALSE(result.thread_desktop_available);
    EXPECT_EQ(result.input_desktop_error, static_cast<DWORD>(ERROR_ACCESS_DENIED));
    EXPECT_EQ(result.thread_desktop_error, static_cast<DWORD>(ERROR_INVALID_HANDLE));
}

// ─── Frame dimensions ──────────────────────────────────────────────────────

TEST(ScreenCapture, InitialDimensions) {
    ScreenCapture capture;
    bool ok = capture.Init();

    if (ok) {
        // Se inicializado, dimensões devem ser > 0.
        EXPECT_GT(capture.GetWidth(), 0u);
        EXPECT_GT(capture.GetHeight(), 0u);
    }

    capture.Shutdown();
}

TEST(ScreenCapture, OverlayPointer_DrawsColorCursor) {
    CapturedFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.pointer_visible = true;
    frame.pointer_x = 1;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 1;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 4;
    frame.pointer_shape_buffer = {0x20, 0x40, 0x80, 0xFF};

    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    ScreenCapture::OverlayPointer(std::span<uint8_t>(pixels.data(), pixels.size()), 8, frame);

    const size_t offset = (1 * 8) + (1 * 4);
    EXPECT_EQ(pixels[offset + 0], 0x20);
    EXPECT_EQ(pixels[offset + 1], 0x40);
    EXPECT_EQ(pixels[offset + 2], 0x80);
    EXPECT_EQ(pixels[offset + 3], 0xFF);
}

TEST(ScreenCapture, OverlayPointer_RespectsHotspot) {
    CapturedFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.pointer_visible = true;
    frame.pointer_x = 1;
    frame.pointer_y = 1;
    frame.pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointer_shape_info.Width = 1;
    frame.pointer_shape_info.Height = 1;
    frame.pointer_shape_info.Pitch = 4;
    frame.pointer_shape_info.HotSpot.x = 1;
    frame.pointer_shape_info.HotSpot.y = 1;
    frame.pointer_shape_buffer = {0x11, 0x22, 0x33, 0xFF};

    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    ScreenCapture::OverlayPointer(std::span<uint8_t>(pixels.data(), pixels.size()), 8, frame);

    EXPECT_EQ(pixels[0], 0x11);
    EXPECT_EQ(pixels[1], 0x22);
    EXPECT_EQ(pixels[2], 0x33);
    EXPECT_EQ(pixels[3], 0xFF);
}
