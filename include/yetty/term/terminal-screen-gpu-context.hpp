#pragma once

namespace yetty {

class GpuAllocator;
class ShaderManager;
class RasterFont;

// TerminalScreen-level GPU context.
// Contains only TerminalScreen's OWN decorations.
// Access parent GPU context via terminalContext.yettyContext.yettyGpuContext
// Access surface dimensions via terminalContext.yettyContext.yettyGpuContext.appGpuContext.surfaceWidth/Height
struct TerminalScreenGpuContext {
    GpuAllocator* allocator = nullptr;      // created/owned by TerminalScreen
    ShaderManager* shaderManager = nullptr; // created/owned by TerminalScreen
    RasterFont* rasterFont = nullptr;       // created/owned by TerminalScreen
};

} // namespace yetty
