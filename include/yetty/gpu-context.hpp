#pragma once

#include <webgpu/webgpu.h>

namespace yetty {

// Low-level GPU context - WebGPU connection handles.
// Created by Yetty, passed to views.
// Note: GpuAllocator is per-view for tracking per-view GPU usage.
struct GPUContext {
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat surfaceFormat;

    // Current render target dimensions (updated before each frame)
    uint32_t renderTargetWidth = 0;
    uint32_t renderTargetHeight = 0;
};

} // namespace yetty
