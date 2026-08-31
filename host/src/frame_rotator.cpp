#include "host/frame_rotator.h"

#include <algorithm>
#include <cstring>

namespace remotex::host {

namespace {

DXGI_MODE_ROTATION NormalizeRotation(DXGI_MODE_ROTATION rotation) {
    return rotation == DXGI_MODE_ROTATION_UNSPECIFIED
        ? DXGI_MODE_ROTATION_IDENTITY
        : rotation;
}

bool RotationSwapsDimensions(DXGI_MODE_ROTATION rotation) {
    return rotation == DXGI_MODE_ROTATION_ROTATE90 ||
           rotation == DXGI_MODE_ROTATION_ROTATE270;
}

void ComputeRotatedDimensions(uint32_t width,
                              uint32_t height,
                              DXGI_MODE_ROTATION rotation,
                              uint32_t& rotated_width,
                              uint32_t& rotated_height) {
    rotated_width = width;
    rotated_height = height;
    if (RotationSwapsDimensions(NormalizeRotation(rotation))) {
        rotated_width = height;
        rotated_height = width;
    }
}

inline uint8_t* PixelPtr(std::vector<uint8_t>& pixels,
                         uint32_t pitch,
                         uint32_t x,
                         uint32_t y) {
    return pixels.data() + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u;
}

inline const uint8_t* PixelPtr(const uint8_t* pixels,
                               uint32_t pitch,
                               uint32_t x,
                               uint32_t y) {
    return pixels + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u;
}

} // namespace

bool FrameRotator::Initialize(ID3D11Device* device) {
    Reset();
    initialized_ = (device != nullptr);
    return initialized_;
}

bool FrameRotator::Rotate(const uint8_t* bgra,
                          uint32_t width,
                          uint32_t height,
                          uint32_t pitch,
                          DXGI_MODE_ROTATION rotation) {
    if (!initialized_ || bgra == nullptr || width == 0 || height == 0 || pitch < width * 4u) {
        return false;
    }

    const DXGI_MODE_ROTATION normalized_rotation = NormalizeRotation(rotation);
    ComputeRotatedDimensions(width, height, normalized_rotation, rotated_width_, rotated_height_);
    rotated_pitch_ = rotated_width_ * 4u;
    rotated_data_.assign(static_cast<size_t>(rotated_pitch_) * rotated_height_, 0);

    if (normalized_rotation == DXGI_MODE_ROTATION_IDENTITY) {
        const size_t row_bytes = static_cast<size_t>(width) * 4u;
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(PixelPtr(rotated_data_, rotated_pitch_, 0, row),
                        PixelPtr(bgra, pitch, 0, row),
                        row_bytes);
        }
        return true;
    }

    for (uint32_t src_y = 0; src_y < height; ++src_y) {
        for (uint32_t src_x = 0; src_x < width; ++src_x) {
            uint32_t dst_x = src_x;
            uint32_t dst_y = src_y;
            switch (normalized_rotation) {
            case DXGI_MODE_ROTATION_ROTATE90:
                dst_x = height - src_y - 1u;
                dst_y = src_x;
                break;
            case DXGI_MODE_ROTATION_ROTATE180:
                dst_x = width - src_x - 1u;
                dst_y = height - src_y - 1u;
                break;
            case DXGI_MODE_ROTATION_ROTATE270:
                dst_x = src_y;
                dst_y = width - src_x - 1u;
                break;
            case DXGI_MODE_ROTATION_IDENTITY:
            case DXGI_MODE_ROTATION_UNSPECIFIED:
            default:
                break;
            }

            std::memcpy(PixelPtr(rotated_data_, rotated_pitch_, dst_x, dst_y),
                        PixelPtr(bgra, pitch, src_x, src_y),
                        4u);
        }
    }

    return true;
}

void FrameRotator::Reset() {
    initialized_ = false;
    rotated_data_.clear();
    rotated_width_ = 0;
    rotated_height_ = 0;
    rotated_pitch_ = 0;
}

uint32_t FrameRotator::GetRotatedWidth() const {
    return rotated_width_;
}

uint32_t FrameRotator::GetRotatedHeight() const {
    return rotated_height_;
}

uint32_t FrameRotator::GetRotatedPitch() const {
    return rotated_pitch_;
}

const uint8_t* FrameRotator::GetRotatedData() const {
    return rotated_data_.empty() ? nullptr : rotated_data_.data();
}

} // namespace remotex::host
