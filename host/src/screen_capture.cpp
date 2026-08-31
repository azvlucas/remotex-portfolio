// RemoteX - host/screen_capture.cpp
// Captura de tela via DXGI Desktop Duplication (DirectX 11).

#include "host/screen_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>

namespace {

uint32_t PointerDrawHeight(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info) {
    return info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
        ? info.Height / 2
        : info.Height;
}

void BlendPointerPixel(uint8_t* dst, const uint8_t* src) {
    const uint32_t alpha = src[3];
    if (alpha == 0) {
        return;
    }
    if (alpha == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        return;
    }

    const uint32_t inv_alpha = 255 - alpha;
    dst[0] = static_cast<uint8_t>((src[0] * alpha + dst[0] * inv_alpha + 127) / 255);
    dst[1] = static_cast<uint8_t>((src[1] * alpha + dst[1] * inv_alpha + 127) / 255);
    dst[2] = static_cast<uint8_t>((src[2] * alpha + dst[2] * inv_alpha + 127) / 255);
    dst[3] = 255;
}

void XorPointerPixel(uint8_t* dst, const uint8_t* src) {
    dst[0] ^= src[0];
    dst[1] ^= src[1];
    dst[2] ^= src[2];
    dst[3] = 255;
}

bool ReadMonochromeMaskBit(const uint8_t* row, uint32_t x) {
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x % 8u));
    return (row[x / 8u] & mask) != 0;
}

bool RotationSwapsDimensions(DXGI_MODE_ROTATION rotation) {
    return rotation == DXGI_MODE_ROTATION_ROTATE90 ||
        rotation == DXGI_MODE_ROTATION_ROTATE270;
}

void OverlayPointerShapeInternal(std::span<uint8_t> bgra_frame,
                                 uint32_t pitch,
                                 uint32_t frame_width,
                                 uint32_t frame_height,
                                 POINT pointer_position,
                                 const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                                 std::span<const uint8_t> pointer_shape_buffer) {
    if (pointer_shape_buffer.empty() || info.Width == 0 || info.Height == 0 || pitch == 0) {
        return;
    }

    const uint32_t draw_height = PointerDrawHeight(info);
    if (draw_height == 0 ||
        bgra_frame.size() < static_cast<size_t>(pitch) * frame_height) {
        return;
    }

    const int left = pointer_position.x;
    const int top = pointer_position.y;
    uint8_t* dst_base = bgra_frame.data();

    for (uint32_t y = 0; y < draw_height; ++y) {
        const int dst_y = top + static_cast<int>(y);
        if (dst_y < 0 || dst_y >= static_cast<int>(frame_height)) {
            continue;
        }

        uint8_t* dst_row = dst_base + static_cast<size_t>(dst_y) * pitch;
        const uint8_t* src_row = pointer_shape_buffer.data() + static_cast<size_t>(y) * info.Pitch;

        if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
            info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            for (uint32_t x = 0; x < info.Width; ++x) {
                const int dst_x = left + static_cast<int>(x);
                if (dst_x < 0 || dst_x >= static_cast<int>(frame_width)) {
                    continue;
                }

                uint8_t* dst_px = dst_row + static_cast<size_t>(dst_x) * 4u;
                const uint8_t* src_px = src_row + static_cast<size_t>(x) * 4u;

                if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR && src_px[3] == 0) {
                    XorPointerPixel(dst_px, src_px);
                } else {
                    BlendPointerPixel(dst_px, src_px);
                }
            }
            continue;
        }

        if (info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
            continue;
        }

        const uint32_t mask_row = draw_height + y;
        if (mask_row >= info.Height) {
            continue;
        }
        const uint8_t* and_row = pointer_shape_buffer.data() + static_cast<size_t>(y) * info.Pitch;
        const uint8_t* xor_row = pointer_shape_buffer.data() + static_cast<size_t>(mask_row) * info.Pitch;

        for (uint32_t x = 0; x < info.Width; ++x) {
            const int dst_x = left + static_cast<int>(x);
            if (dst_x < 0 || dst_x >= static_cast<int>(frame_width)) {
                continue;
            }

            const bool and_bit = ReadMonochromeMaskBit(and_row, x);
            const bool xor_bit = ReadMonochromeMaskBit(xor_row, x);
            uint8_t* dst_px = dst_row + static_cast<size_t>(dst_x) * 4u;

            if (!and_bit && !xor_bit) {
                dst_px[0] = 0;
                dst_px[1] = 0;
                dst_px[2] = 0;
                dst_px[3] = 255;
            } else if (!and_bit && xor_bit) {
                dst_px[0] = 255;
                dst_px[1] = 255;
                dst_px[2] = 255;
                dst_px[3] = 255;
            } else if (and_bit && xor_bit) {
                dst_px[0] ^= 0xFF;
                dst_px[1] ^= 0xFF;
                dst_px[2] ^= 0xFF;
                dst_px[3] = 255;
            }
        }
    }
}

void ComputeLogicalDimensions(uint32_t source_width,
                              uint32_t source_height,
                              DXGI_MODE_ROTATION rotation,
                              uint32_t& logical_width,
                              uint32_t& logical_height) {
    logical_width = source_width;
    logical_height = source_height;
    if (RotationSwapsDimensions(rotation)) {
        std::swap(logical_width, logical_height);
    }
}

} // namespace

namespace remotex::host {

ScreenCapture::~ScreenCapture() {
    Shutdown();
}

bool ScreenCapture::Init(uint32_t adapter_index, uint32_t output_index) {
    if (initialized_) {
        Shutdown();
    }

    adapter_index_ = adapter_index;
    output_index_ = output_index;

    if (!CreateD3D11Device(adapter_index)) {
        return false;
    }

    if (!CreateOutputDuplication(adapter_index, output_index)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width = dup_desc_.ModeDesc.Width;
    tex_desc.Height = dup_desc_.ModeDesc.Height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = dup_desc_.ModeDesc.Format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_STAGING;
    tex_desc.BindFlags = 0;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    const HRESULT hr = device_->CreateTexture2D(&tex_desc, nullptr, staging_texture_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    initialized_ = true;
    return true;
}

void ScreenCapture::Shutdown() {
    ReleaseFrame();
    staging_texture_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    dup_desc_ = {};
    output_desc_ = {};
    rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    logical_width_ = 0;
    logical_height_ = 0;
    frame_count_ = 0;
    initialized_ = false;
    has_pointer_shape_ = false;
    pointer_visible_ = false;
    pointer_position_ = {};
    pointer_shape_info_ = {};
    pointer_shape_buffer_.clear();
    last_desktop_context_ = {};
    has_last_desktop_context_ = false;
}

bool ScreenCapture::Reinitialize() {
    Shutdown();
    return Init(adapter_index_, output_index_);
}

std::vector<MonitorInfo> ScreenCapture::EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        return monitors;
    }

    for (uint32_t ai = 0; ; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(ai, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        for (uint32_t oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(oi, output.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;

            DXGI_OUTPUT_DESC desc{};
            hr = output->GetDesc(&desc);
            if (FAILED(hr)) continue;

            if (!desc.AttachedToDesktop) continue;

            MonitorInfo info{};
            info.adapter_index = ai;
            info.output_index = oi;
            info.left   = desc.DesktopCoordinates.left;
            info.top    = desc.DesktopCoordinates.top;
            info.right  = desc.DesktopCoordinates.right;
            info.bottom = desc.DesktopCoordinates.bottom;
            info.width  = static_cast<uint32_t>(info.right - info.left);
            info.height = static_cast<uint32_t>(info.bottom - info.top);
            info.primary = (info.left == 0 && info.top == 0);

            // Convert wide device name to UTF-8.
            int len = WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                std::string name(static_cast<size_t>(len - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, name.data(), len, nullptr, nullptr);
                // Strip leading "\\.\" prefix if present.
                if (name.rfind("\\\\.\\", 0) == 0) {
                    name = name.substr(4);
                }
                info.device_name = std::move(name);
            }

            monitors.push_back(std::move(info));
        }
    }

    return monitors;
}

bool ScreenCapture::CreateD3D11Device(uint32_t adapter_index) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL achieved_level{};
    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        device_.ReleaseAndGetAddressOf(),
        &achieved_level,
        context_.ReleaseAndGetAddressOf());

    return SUCCEEDED(hr);
}

bool ScreenCapture::CreateOutputDuplication(uint32_t /*adapter_index*/, uint32_t output_index) {
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(output_index, output.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    hr = output->GetDesc(&output_desc_);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        return false;
    }

    hr = output1->DuplicateOutput(device_.Get(), duplication_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    duplication_->GetDesc(&dup_desc_);
    rotation_ = output_desc_.Rotation;
    ComputeLogicalDimensions(
        dup_desc_.ModeDesc.Width,
        dup_desc_.ModeDesc.Height,
        rotation_,
        logical_width_,
        logical_height_);
    return true;
}

bool ScreenCapture::RefreshCurrentOutputDesc(DXGI_OUTPUT_DESC& desc) const {
    if (!device_) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(output_index_, output.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    hr = output->GetDesc(&desc);
    return SUCCEEDED(hr);
}

bool ScreenCapture::DetectDisplayChange() {
    DXGI_OUTPUT_DESC current_desc{};
    if (!RefreshCurrentOutputDesc(current_desc)) {
        return false;
    }

    const bool bounds_changed =
        current_desc.DesktopCoordinates.left != output_desc_.DesktopCoordinates.left ||
        current_desc.DesktopCoordinates.top != output_desc_.DesktopCoordinates.top ||
        current_desc.DesktopCoordinates.right != output_desc_.DesktopCoordinates.right ||
        current_desc.DesktopCoordinates.bottom != output_desc_.DesktopCoordinates.bottom;
    const bool rotation_changed = current_desc.Rotation != output_desc_.Rotation;
    if (!bounds_changed && !rotation_changed) {
        return false;
    }

    output_desc_ = current_desc;
    rotation_ = output_desc_.Rotation;
    logical_width_ = static_cast<uint32_t>(
        (std::max)(0L, output_desc_.DesktopCoordinates.right - output_desc_.DesktopCoordinates.left));
    logical_height_ = static_cast<uint32_t>(
        (std::max)(0L, output_desc_.DesktopCoordinates.bottom - output_desc_.DesktopCoordinates.top));
    return true;
}

void ScreenCapture::PopulateDesktopContextResult(CaptureResult& result,
                                                 const DesktopContextSnapshot* previous,
                                                 const DesktopContextSnapshot& current) {
    result.previous_input_desktop_name = previous != nullptr
        ? previous->input_desktop_name
        : std::string{};
    result.input_desktop_name = current.input_desktop_name;
    result.thread_desktop_name = current.thread_desktop_name;
    result.input_desktop_error = current.input_error;
    result.thread_desktop_error = current.thread_error;
    result.input_desktop_available = current.input_desktop_available;
    result.thread_desktop_available = current.thread_desktop_available;
    result.input_matches_thread = current.InputMatchesThread();
    result.input_desktop_changed =
        previous != nullptr &&
        previous->input_desktop_available &&
        current.input_desktop_available &&
        previous->input_desktop_name != current.input_desktop_name;
    result.thread_desktop_changed =
        previous != nullptr &&
        previous->thread_desktop_available &&
        current.thread_desktop_available &&
        previous->thread_desktop_name != current.thread_desktop_name;
}

CaptureStatus ScreenCapture::ClassifyAcquireFailure(HRESULT hr,
                                                    const DesktopContextSnapshot* previous,
                                                    const DesktopContextSnapshot& current,
                                                    CaptureResult& result) {
    PopulateDesktopContextResult(result, previous, current);

    const bool desktop_context_unavailable =
        !current.input_desktop_available || !current.thread_desktop_available;
    const bool desktop_mismatch =
        current.HasCompleteContext() && !current.InputMatchesThread();
    const bool desktop_switched =
        result.input_desktop_changed ||
        result.thread_desktop_changed ||
        desktop_mismatch;

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (desktop_context_unavailable) {
            return CaptureStatus::DesktopContextUnavailable;
        }
        if (desktop_mismatch) {
            return CaptureStatus::DesktopSwitched;
        }
        return CaptureStatus::Timeout;
    }
    if (desktop_context_unavailable && hr == DXGI_ERROR_ACCESS_LOST) {
        return CaptureStatus::DesktopContextUnavailable;
    }
    if (desktop_switched && hr == DXGI_ERROR_ACCESS_LOST) {
        return CaptureStatus::DesktopSwitched;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        return CaptureStatus::AccessDenied;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        return CaptureStatus::DeviceLost;
    }
    return FAILED(hr) ? CaptureStatus::Error : CaptureStatus::Ok;
}

CaptureResult ScreenCapture::CaptureFrame(CapturedFrame& frame, uint32_t timeout_ms) {
    CaptureResult result;
    if (!initialized_ || !duplication_) {
        return result;
    }

    const DesktopContextSnapshot current_desktop_context = CaptureDesktopContext();
    PopulateDesktopContextResult(
        result,
        has_last_desktop_context_ ? &last_desktop_context_ : nullptr,
        current_desktop_context);

    ReleaseFrame();
    if (DetectDisplayChange()) {
        result.status = CaptureStatus::DisplayChanged;
        if (current_desktop_context.HasAnyDesktop()) {
            last_desktop_context_ = current_desktop_context;
            has_last_desktop_context_ = true;
        }
        return result;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> desktop_resource;
    const auto acquire_started = std::chrono::steady_clock::now();
    HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &frame_info, desktop_resource.GetAddressOf());
    result.acquire_wait_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - acquire_started).count();

    const CaptureStatus acquire_status = ClassifyAcquireFailure(
        hr,
        has_last_desktop_context_ ? &last_desktop_context_ : nullptr,
        current_desktop_context,
        result);
    if (current_desktop_context.HasAnyDesktop()) {
        last_desktop_context_ = current_desktop_context;
        has_last_desktop_context_ = true;
    } else {
        last_desktop_context_ = {};
        has_last_desktop_context_ = false;
    }

    if (acquire_status != CaptureStatus::Ok) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            duplication_.Reset();
        }
        result.status = acquire_status;
        result.timed_out = (hr == DXGI_ERROR_WAIT_TIMEOUT);
        return result;
    }

    frame_acquired_ = true;
    result.status = CaptureStatus::Ok;
    result.accumulated_frames = frame_info.AccumulatedFrames;

    ComPtr<ID3D11Texture2D> desktop_tex;
    hr = desktop_resource.As(&desktop_tex);
    if (FAILED(hr)) {
        ReleaseFrame();
        result.status = CaptureStatus::Error;
        return result;
    }

    context_->CopyResource(staging_texture_.Get(), desktop_tex.Get());

    frame.dirty_rects.clear();
    frame.move_rects.clear();

    if (frame_info.TotalMetadataBufferSize > 0) {
        UINT move_buf_size = 0;
        duplication_->GetFrameMoveRects(0, nullptr, &move_buf_size);
        if (move_buf_size > 0) {
            const UINT move_count = move_buf_size / sizeof(DXGI_OUTDUPL_MOVE_RECT);
            frame.move_rects.resize(move_count);
            hr = duplication_->GetFrameMoveRects(move_buf_size, frame.move_rects.data(), &move_buf_size);
            if (FAILED(hr)) {
                frame.move_rects.clear();
            }
        }

        UINT dirty_buf_size = 0;
        duplication_->GetFrameDirtyRects(0, nullptr, &dirty_buf_size);
        if (dirty_buf_size > 0) {
            const UINT rect_count = dirty_buf_size / sizeof(RECT);
            std::vector<RECT> raw_rects(rect_count);
            hr = duplication_->GetFrameDirtyRects(dirty_buf_size, raw_rects.data(), &dirty_buf_size);
            if (SUCCEEDED(hr)) {
                const UINT actual_count = dirty_buf_size / sizeof(RECT);
                frame.dirty_rects.reserve(actual_count);
                for (UINT i = 0; i < actual_count; ++i) {
                    frame.dirty_rects.push_back(DirtyRect{
                        .x = raw_rects[i].left,
                        .y = raw_rects[i].top,
                        .width = raw_rects[i].right - raw_rects[i].left,
                        .height = raw_rects[i].bottom - raw_rects[i].top,
                    });
                }
            }
        }
    }

    pointer_visible_ = frame_info.PointerPosition.Visible != FALSE;
    pointer_position_ = frame_info.PointerPosition.Position;

    if (frame_info.PointerShapeBufferSize > 0 || (pointer_visible_ && !has_pointer_shape_)) {
        UINT required = frame_info.PointerShapeBufferSize;
        pointer_shape_buffer_.resize(required);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_info{};
        hr = duplication_->GetFramePointerShape(required,
                                                required == 0 ? nullptr : pointer_shape_buffer_.data(),
                                                &required,
                                                &pointer_info);
        if (hr == DXGI_ERROR_MORE_DATA && required > pointer_shape_buffer_.size()) {
            pointer_shape_buffer_.resize(required);
            hr = duplication_->GetFramePointerShape(required,
                                                    pointer_shape_buffer_.data(),
                                                    &required,
                                                    &pointer_info);
        }
        if (SUCCEEDED(hr)) {
            pointer_shape_buffer_.resize(required);
            pointer_shape_info_ = pointer_info;
            has_pointer_shape_ = !pointer_shape_buffer_.empty();
        }
    }

    frame.texture = staging_texture_;
    frame.width = dup_desc_.ModeDesc.Width;
    frame.height = dup_desc_.ModeDesc.Height;
    frame.frame_index = ++frame_count_;
    frame.pointer_visible = pointer_visible_;
    frame.pointer_x = pointer_position_.x;
    frame.pointer_y = pointer_position_.y;
    frame.pointer_shape_info = pointer_shape_info_;
    if (has_pointer_shape_) {
        frame.pointer_shape_buffer = pointer_shape_buffer_;
    } else {
        frame.pointer_shape_buffer.clear();
    }

    return result;
}

void ScreenCapture::OverlayPointer(std::span<uint8_t> bgra_frame,
                                   uint32_t pitch,
                                   const CapturedFrame& frame) {
    if (!frame.pointer_visible || frame.pointer_shape_buffer.empty()) {
        return;
    }

    const POINT draw_origin{
        frame.pointer_x - frame.pointer_shape_info.HotSpot.x,
        frame.pointer_y - frame.pointer_shape_info.HotSpot.y,
    };
    OverlayPointerShapeInternal(
        bgra_frame,
        pitch,
        frame.width,
        frame.height,
        draw_origin,
        frame.pointer_shape_info,
        std::span<const uint8_t>(frame.pointer_shape_buffer.data(), frame.pointer_shape_buffer.size()));
}

void ScreenCapture::OverlayPointerShape(std::span<uint8_t> bgra_frame,
                                        uint32_t pitch,
                                        uint32_t frame_width,
                                        uint32_t frame_height,
                                        POINT pointer_position,
                                        const DXGI_OUTDUPL_POINTER_SHAPE_INFO& pointer_shape_info,
                                        std::span<const uint8_t> pointer_shape_buffer) {
    OverlayPointerShapeInternal(
        bgra_frame,
        pitch,
        frame_width,
        frame_height,
        pointer_position,
        pointer_shape_info,
        pointer_shape_buffer);
}

void ScreenCapture::ReleaseFrame() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
}

} // namespace remotex::host
