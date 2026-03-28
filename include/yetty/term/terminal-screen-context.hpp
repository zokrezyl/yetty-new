#pragma once

#include <yetty/core/types.hpp>
#include <yetty/yetty-context.hpp>

namespace yetty {

class Pty;
class ShaderManager;
class FontManager;
class CardFactory;
class GpuMemoryManager;

// Per-TerminalScreen context for GPU rendering.
// Extends YettyContext with per-screen managers.
// GpuAllocator is accessed via yettyContext.gpuContext.allocator
struct TerminalScreenContext {
  YettyContext yettyContext;
  Pty *pty = nullptr;

  ShaderManager *shaderManager = nullptr;
  FontManager *fontManager = nullptr;
  CardFactory *cardFactory = nullptr;
  GpuMemoryManager *cardManager = nullptr;

  core::ObjectId screenId = 0;
};

} // namespace yetty
