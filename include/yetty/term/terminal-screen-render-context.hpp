#pragma once

#include <yetty/term/terminal-screen-context.hpp>
#include <webgpu/webgpu.h>

namespace yetty {

// Render context for TerminalScreen layers.
// Created each frame by TerminalScreen, passed by const ref, stored by value.
struct TerminalScreenRenderContext {
    TerminalScreenContext terminalScreenContext;  // COPY
    WGPURenderPassEncoder pass = nullptr;         // per-frame
};

} // namespace yetty
