#pragma once

#include <yetty/yetty-context.hpp>

namespace yetty {

class Pty;

namespace core {
class EventLoop;
}

// Terminal-level context.
// Contains copy of YettyContext plus Terminal-specific objects.
struct TerminalContext {
    YettyContext yettyContext;             // COPY of Yetty's context
    core::EventLoop* eventLoop = nullptr;  // owned by Terminal
    Pty* pty = nullptr;                    // owned by Terminal
};

} // namespace yetty
