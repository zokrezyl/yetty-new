// WebAssembly surface.cpp - Platform-specific surface creation
//
// Provides createSurface(instance) - creates WGPUSurface from canvas HTML selector.
// Yetty creates instance, calls this, then creates adapter/device/queue.

#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/html5.h>

namespace yetty {
namespace platform {
namespace webasm {

WGPUSurface createSurface(WGPUInstance instance) {
    if (!instance) {
        yerror("createSurface: null instance");
        return nullptr;
    }

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource = {};
    canvasSource.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvasSource.selector = { .data = "#canvas", .length = 7 };

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = &canvasSource.chain;

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);
    if (!surface) {
        yerror("createSurface: Failed to create surface from #canvas");
        return nullptr;
    }
    ydebug("createSurface: Surface created from #canvas");

    return surface;
}

} // namespace webasm
} // namespace platform
} // namespace yetty
