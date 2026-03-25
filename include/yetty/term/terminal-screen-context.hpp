#pragma once

#include <yetty/core/types.hpp>
#include <yetty/yetty-context.hpp>

namespace yetty {

class GpuAllocator;
class ShaderManager;
class FontManager;
class CardFactory;
class GpuMemoryManager;

// Per-GPUScreen context passed to cards and other per-screen components.
// Created by GPUScreen, extends YettyContext with per-screen managers.
struct GpuScreenContext {
  YettyContext *yettyCtx = nullptr;

  GpuAllocator *gpuAllocator = nullptr;
  ShaderManager *shaderManager = nullptr;
  FontManager *fontManager = nullptr;
  CardFactory *cardFactory = nullptr;
  GpuMemoryManager *cardManager = nullptr;

  core::ObjectId screenId = 0;
};

} // namespace yetty
