#pragma once

#include <yetty/render-target.hpp>
#include <yetty/yetty-gpu-context.hpp>
#include <webgpu/webgpu.h>
#include <cstdint>
#include <string>

namespace yetty {

// Render context for TerminalScreen layers.
// Created each frame by TerminalScreen, passed by const ref, stored by value.
struct TerminalScreenRenderContext {
    // GPU handles
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    YettyGpuContext gpuContext;

    // Per-frame
    WGPURenderPassEncoder pass = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    // Config
    std::string shadersDir;
};

} // namespace yetty
