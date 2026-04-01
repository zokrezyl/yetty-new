#pragma once

#include <webgpu/webgpu.h>
#include <cstdint>

namespace yetty {

struct RenderTarget {
    WGPUTextureView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace yetty
