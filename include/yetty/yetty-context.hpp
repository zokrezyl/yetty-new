#pragma once

#include <yetty/app-context.hpp>
#include <yetty/core/types.hpp>
#include <yetty/gpu-context.hpp>

namespace yetty {

class GpuAllocator;
class GpuMonitor;
class MsMsdfFont;
class GpuMemoryManager;
class YGuiOverlay;

// Yetty-level context — created by Yetty, passed to GPUScreens.
// Extends AppContext with GPU state and shared managers.
struct YettyContext {
  AppContext *appCtx = nullptr;

  GPUContext gpuCtx;

  GpuAllocator *gpuAllocator = nullptr;
  GpuMonitor *gpuMonitor = nullptr;
  YGuiOverlay *yguiOverlay = nullptr;
  MsMsdfFont *defaultMsMsdfFont = nullptr;
  GpuMemoryManager *cardManager = nullptr;

  core::ObjectId screenId = 0;
};

} // namespace yetty
