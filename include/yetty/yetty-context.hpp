#pragma once

#include <yetty/app-context.hpp>
#include <yetty/core/types.hpp>
#include <yetty/gpu-context.hpp>

namespace yetty {

class GpuMonitor;
class MsMsdfFont;
class GpuMemoryManager;
class YGuiOverlay;
class SharedBindGroup;

// Yetty-level context — created by Yetty, passed to Views.
// Contains GPU connection and shared resources.
// Note: GpuAllocator is per-view, not here.
// Note: ShaderManager is per-view, not here.
struct YettyContext {
  AppContext *appContext = nullptr;

  GPUContext gpuContext;

  // Shared resources (large objects shared across views)
  SharedBindGroup *sharedBindGroup = nullptr;
  MsMsdfFont *defaultMsMsdfFont = nullptr;

  // Legacy - to be reviewed
  GpuMonitor *gpuMonitor = nullptr;
  YGuiOverlay *yguiOverlay = nullptr;
  GpuMemoryManager *cardManager = nullptr;

  core::ObjectId screenId = 0;
};

} // namespace yetty
