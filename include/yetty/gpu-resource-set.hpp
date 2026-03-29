#pragma once

#include <webgpu/webgpu.h>
#include <string>

namespace yetty {

// Simple struct for GPU resources that can be bound to shaders.
// Stack-allocated, copied by value.
// ShaderManager uses this to generate WGSL bindings and create bind groups.
struct GpuResourceSet {
    bool shared = false;          // true = bind group 0, false = bind group 1
    std::string name;             // e.g., "msdfFont", "rasterFont", "cells"

    // Optional texture
    WGPUTextureView texture = nullptr;
    std::string textureWgslType;  // e.g., "texture_2d<f32>"

    // Optional sampler
    WGPUSampler sampler = nullptr;

    // Optional buffer
    WGPUBuffer buffer = nullptr;
    size_t bufferSize = 0;
    std::string bufferWgslType;   // e.g., "array<TextCell>", "array<GlyphUV>"
    bool bufferReadonly = true;

    // Count non-null bindings
    uint32_t bindingCount() const {
        uint32_t count = 0;
        if (texture) ++count;
        if (sampler) ++count;
        if (buffer) ++count;
        return count;
    }
};

} // namespace yetty
