#pragma once

#include <webgpu/webgpu.h>
#include <string>
#include <vector>

namespace yetty {

// Describes one uniform field to be packed into combined uniform buffer
struct UniformField {
    std::string name;       // e.g., "fontCellSize"
    std::string wgslType;   // e.g., "vec2<f32>", "u32", "f32"
    size_t size = 0;        // bytes
};

// Describes GPU resources an object needs. NO GPU handles - just descriptions.
// Filled by object (Font, etc.), consumed by GpuResourceBinder and ShaderManager.
struct GpuResourceSet {
    bool shared = false;          // true = bind group 0, false = bind group 1
    std::string name;             // e.g., "rasterFont", "cells"

    // Texture description
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    WGPUTextureFormat textureFormat = WGPUTextureFormat_Undefined;
    std::string textureWgslType;  // e.g., "texture_2d<f32>"

    // Sampler description
    WGPUFilterMode samplerFilter = WGPUFilterMode_Nearest;

    // Buffer description
    size_t bufferSize = 0;
    std::string bufferWgslType;   // e.g., "array<RasterGlyphUV>"
    bool bufferReadonly = true;

    // Uniform fields (packed into ONE combined uniform buffer)
    std::vector<UniformField> uniformFields;

    // CPU data pointers (for GpuResourceBinder to upload)
    const uint8_t* textureData = nullptr;
    size_t textureDataSize = 0;
    const uint8_t* bufferData = nullptr;
    size_t bufferDataSize = 0;
    const uint8_t* uniformData = nullptr;  // packed uniform data matching uniformFields
    size_t uniformDataSize = 0;
};

} // namespace yetty
