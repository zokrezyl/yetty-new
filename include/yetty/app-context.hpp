#pragma once

#include <yetty/app-gpu-context.hpp>
#include <string>

namespace yetty {

class Config;
class PtyFactory;
class ClipboardManager;

namespace core {
class PlatformInputPipe;
} // namespace core

// Application-level context created by platform main() and passed to Yetty::create.
// Contains platform-specific objects and configuration.
struct AppContext {
    AppGpuContext appGpuContext;  // platform GPU objects (instance, surface)

    Config* config = nullptr;
    core::PlatformInputPipe* platformInputPipe = nullptr;
    ClipboardManager* clipboardManager = nullptr;
    PtyFactory* ptyFactory = nullptr;
};

} // namespace yetty
