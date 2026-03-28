#pragma once

#include <webgpu/webgpu.h>

namespace yetty {

class Config;
class PtyFactory;
class ClipboardManager;

namespace core {
class PlatformInputPipe;
} // namespace core

// Application-level context created by platform main() and passed to yetty::create.
// Raw pointers — the app owns these objects and outlives all children.
struct AppContext {
  Config *config = nullptr;
  core::PlatformInputPipe *platformInputPipe = nullptr;
  ClipboardManager *clipboardManager = nullptr;
  PtyFactory *ptyFactory = nullptr;

  // WebGPU surface - created by platform (requires window handle)
  // Instance is created internally by Yetty
  WGPUSurface surface = nullptr;
};

} // namespace yetty
