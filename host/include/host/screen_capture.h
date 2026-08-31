#pragma once
// RemoteX - host/screen_capture.h
// Captura de tela via DXGI Desktop Duplication (DirectX 11).

#include "host/desktop_context.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace remotex::host {

using Microsoft::WRL::ComPtr;

enum class CaptureStatus {
    Ok,
    Timeout,
    DisplayChanged,
    DesktopSwitched,
    DesktopContextUnavailable,
    DeviceLost,
    AccessDenied,
    Error
};

struct CaptureResult {
    CaptureStatus status = CaptureStatus::Error;
    double acquire_wait_ms = 0.0;
    uint32_t accumulated_frames = 0;
    bool timed_out = false;
    std::string previous_input_desktop_name;
    std::string input_desktop_name;
    std::string thread_desktop_name;
    DWORD input_desktop_error = ERROR_SUCCESS;
    DWORD thread_desktop_error = ERROR_SUCCESS;
    bool input_desktop_available = false;
    bool thread_desktop_available = false;
    bool input_matches_thread = false;
    bool input_desktop_changed = false;
    bool thread_desktop_changed = false;
};

struct DirtyRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct CapturedFrame {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t                width = 0;
    uint32_t                height = 0;
    uint64_t                frame_index = 0;
    std::vector<DirtyRect>  dirty_rects;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> move_rects;
    bool                    pointer_visible = false;
    int32_t                 pointer_x = 0;
    int32_t                 pointer_y = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_shape_info{};
    std::vector<uint8_t>    pointer_shape_buffer;
};

struct MonitorInfo {
    uint32_t    adapter_index;
    uint32_t    output_index;
    std::string device_name;
    LONG        left, top, right, bottom;
    uint32_t    width;
    uint32_t    height;
    bool        primary;
};

class ScreenCapture {
public:
    friend class ScreenCaptureRotationTestAccess;
    friend class ScreenCaptureDesktopContextTestAccess;

    ScreenCapture() = default;
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;
    ScreenCapture(ScreenCapture&&) = delete;
    ScreenCapture& operator=(ScreenCapture&&) = delete;

    bool Init(uint32_t adapter_index = 0, uint32_t output_index = 0);
    CaptureResult CaptureFrame(CapturedFrame& frame, uint32_t timeout_ms = 16);
    void Shutdown();

    static void OverlayPointer(std::span<uint8_t> bgra_frame,
                               uint32_t pitch,
                               const CapturedFrame& frame);
    static void OverlayPointerShape(std::span<uint8_t> bgra_frame,
                                    uint32_t pitch,
                                    uint32_t frame_width,
                                    uint32_t frame_height,
                                    POINT pointer_position,
                                    const DXGI_OUTDUPL_POINTER_SHAPE_INFO& pointer_shape_info,
                                    std::span<const uint8_t> pointer_shape_buffer);

    uint32_t GetWidth() const { return logical_width_; }
    uint32_t GetHeight() const { return logical_height_; }
    DXGI_MODE_ROTATION GetRotation() const { return rotation_; }
    DXGI_FORMAT GetFormat() const { return dup_desc_.ModeDesc.Format; }
    uint32_t GetAdapterIndex() const { return adapter_index_; }
    uint32_t GetOutputIndex() const { return output_index_; }

    ID3D11Device* GetDevice() const { return device_.Get(); }
    ID3D11DeviceContext* GetContext() const { return context_.Get(); }
    const DXGI_OUTPUT_DESC& GetOutputDesc() const { return output_desc_; }

    bool Reinitialize();

    static std::vector<MonitorInfo> EnumerateMonitors();

private:
    static void PopulateDesktopContextResult(CaptureResult& result,
                                             const DesktopContextSnapshot* previous,
                                             const DesktopContextSnapshot& current);
    static CaptureStatus ClassifyAcquireFailure(HRESULT hr,
                                                const DesktopContextSnapshot* previous,
                                                const DesktopContextSnapshot& current,
                                                CaptureResult& result);

    bool CreateD3D11Device(uint32_t adapter_index);
    bool CreateOutputDuplication(uint32_t adapter_index, uint32_t output_index);
    bool RefreshCurrentOutputDesc(DXGI_OUTPUT_DESC& desc) const;
    bool DetectDisplayChange();
    void ReleaseFrame();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_texture_;

    DXGI_OUTDUPL_DESC dup_desc_{};
    DXGI_OUTPUT_DESC output_desc_{};
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    uint32_t logical_width_ = 0;
    uint32_t logical_height_ = 0;

    uint32_t adapter_index_ = 0;
    uint32_t output_index_ = 0;
    uint64_t frame_count_ = 0;
    bool frame_acquired_ = false;
    bool initialized_ = false;
    bool has_pointer_shape_ = false;
    bool pointer_visible_ = false;
    POINT pointer_position_{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_shape_info_{};
    std::vector<uint8_t> pointer_shape_buffer_;
    DesktopContextSnapshot last_desktop_context_{};
    bool has_last_desktop_context_ = false;
};

} // namespace remotex::host
