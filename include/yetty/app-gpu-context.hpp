#pragma once

#include <webgpu/webgpu.h>

namespace yetty {

// Platform-level GPU context.
// Created by platform main(), contains only platform-specific GPU objects.
struct AppGpuContext {
    WGPUInstance instance = nullptr;  // created by platform
    WGPUSurface surface = nullptr;    // created by platform (null for headless)
};

} // namespace yetty
