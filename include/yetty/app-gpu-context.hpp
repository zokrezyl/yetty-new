#pragma once

#include <webgpu/webgpu.h>

namespace yetty {

// Platform-level GPU context.
// Created by platform main(), contains only platform-specific GPU objects.
struct AppGpuContext {
    WGPUInstance instance = nullptr;  // created by platform
    WGPUSurface surface = nullptr;    // created by platform (null for headless)

    // Surface dimensions in actual pixels (not screen coordinates).
    // On HiDPI displays: surfaceWidth/Height = windowWidth/Height * contentScale.
    // These come from glfwGetFramebufferSize() and are used for wgpuSurfaceConfigure().
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
};

} // namespace yetty
