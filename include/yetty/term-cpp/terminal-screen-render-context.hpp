#pragma once

#include <yetty/term/terminal-screen-context.hpp>
#include <webgpu/webgpu.h>

namespace yetty {

// Render context for TerminalScreen layers.
// Created each frame by TerminalScreen, passed by const ref, stored by value.
struct TerminalScreenRenderContext {
    TerminalScreenContext terminalScreenContext;  // COPY
    WGPURenderPassEncoder pass = nullptr;         // per-frame
    uint32_t surfaceWidth = 0;                    // current surface size (updated on resize)
    uint32_t surfaceHeight = 0;
};

} // namespace yetty
