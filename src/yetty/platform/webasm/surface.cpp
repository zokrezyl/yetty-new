// WebAssembly surface.cpp - WebGPU surface from HTML canvas
//
// Creates WGPUSurface from the #canvas HTML element.

#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/html5.h>

namespace yetty {
namespace platform {
namespace webasm {

WGPUSurface createSurface() {
    // Create instance (required by wgpuInstanceCreateSurface)
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        yerror("createSurface: Failed to create WebGPU instance");
        return nullptr;
    }

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource = {};
    canvasSource.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvasSource.selector = { .data = "#canvas", .length = 7 };

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = &canvasSource.chain;

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);

    // Release instance - surface retains what it needs internally
    wgpuInstanceRelease(instance);

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
