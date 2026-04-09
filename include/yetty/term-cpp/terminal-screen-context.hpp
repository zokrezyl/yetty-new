#pragma once

#include <yetty/term/terminal-context.hpp>
#include <yetty/term/terminal-screen-gpu-context.hpp>

namespace yetty {

// TerminalScreen-level context.
// Contains copy of TerminalContext plus TerminalScreen-specific state.
struct TerminalScreenContext {
    TerminalContext terminalContext;                        // COPY of Terminal's context
    TerminalScreenGpuContext terminalScreenGpuContext;      // TerminalScreen's GPU state
};

} // namespace yetty
