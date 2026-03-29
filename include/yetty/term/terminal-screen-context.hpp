#pragma once

#include <yetty/yetty-context.hpp>
#include <yetty/term/terminal-screen-gpu-context.hpp>

namespace yetty {

class Pty;

// TerminalScreen-level context.
// Contains copy of YettyContext plus terminal-specific state.
struct TerminalScreenContext {
    YettyContext yettyContext;                  // COPY of Yetty's context
    TerminalScreenGpuContext gpuContext;        // TerminalScreen's GPU state
    Pty* pty = nullptr;                         // owned by Terminal, used by TerminalScreen
};

} // namespace yetty
