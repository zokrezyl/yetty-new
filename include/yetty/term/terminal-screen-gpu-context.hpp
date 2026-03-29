#pragma once

#include <yetty/yetty-gpu-context.hpp>

namespace yetty {

class GpuAllocator;
class ShaderManager;
class RasterFont;

// TerminalScreen-level GPU context.
// Created by TerminalScreen, contains per-view GPU resources.
struct TerminalScreenGpuContext {
    YettyGpuContext yettyGpuContext;      // copy of Yetty's GPU context
    GpuAllocator* allocator = nullptr;     // created/owned by TerminalScreen
    ShaderManager* shaderManager = nullptr; // created/owned by TerminalScreen
    RasterFont* rasterFont = nullptr;       // created/owned by TerminalScreen

    // Render target dimensions (updated on resize)
    uint32_t renderTargetWidth = 0;
    uint32_t renderTargetHeight = 0;
};

} // namespace yetty
