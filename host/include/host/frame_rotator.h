#pragma once

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace remotex::host {

class FrameRotator {
public:
    bool Initialize(ID3D11Device* device);
    bool Rotate(const uint8_t* bgra,
                uint32_t width,
                uint32_t height,
                uint32_t pitch,
                DXGI_MODE_ROTATION rotation);
    void Reset();
    uint32_t GetRotatedWidth() const;
    uint32_t GetRotatedHeight() const;
    uint32_t GetRotatedPitch() const;
    const uint8_t* GetRotatedData() const;

private:
    bool initialized_ = false;
    std::vector<uint8_t> rotated_data_;
    uint32_t rotated_width_ = 0;
    uint32_t rotated_height_ = 0;
    uint32_t rotated_pitch_ = 0;
};

} // namespace remotex::host
