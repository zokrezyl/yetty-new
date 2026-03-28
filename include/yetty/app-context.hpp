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

  // WebGPU - instance created by platform (needed for surface), reused by Yetty
  WGPUInstance instance = nullptr;
  WGPUSurface surface = nullptr;
};

} // namespace yetty
