#pragma once

#include <yetty/app-gpu-context.hpp>
#include <webgpu/webgpu.h>

namespace yetty {

// Yetty-level GPU context.
// Created by Yetty, contains adapter/device/queue.
struct YettyGpuContext {
    AppGpuContext appGpuContext;          // copy of platform's GPU context
    WGPUAdapter adapter = nullptr;         // created by Yetty
    WGPUDevice device = nullptr;           // created by Yetty
    WGPUQueue queue = nullptr;             // created by Yetty
    WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
};

} // namespace yetty
