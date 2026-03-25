// WebAssembly surface.cpp - WebGPU instance and surface creation
//
// Platform creates (on the single thread):
//   - WGPUInstance
//   - WGPUSurface (from instance + canvas HTML selector)
//
// These are passed to Yetty, which creates adapter/device/queue.
//
// Platform owns instance and surface lifetime - destroys them after Yetty exits.

#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/html5.h>

namespace yetty {
namespace platform {
namespace webasm {

// Holds instance + surface created by platform
struct WebGPUContext {
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
};

WebGPUContext* createWebGPUContext() {
    auto* ctx = new WebGPUContext();

    // Create instance
    WGPUInstanceDescriptor instanceDesc = {};
    ctx->instance = wgpuCreateInstance(&instanceDesc);
    if (!ctx->instance) {
        yerror("Surface: Failed to create WebGPU instance");
        delete ctx;
        return nullptr;
    }
    ydebug("Surface: Instance created");

    // Create surface from canvas HTML selector
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource = {};
    canvasSource.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvasSource.selector = { .data = "#canvas", .length = 7 };

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = &canvasSource.chain;

    ctx->surface = wgpuInstanceCreateSurface(ctx->instance, &surfaceDesc);
    if (!ctx->surface) {
        yerror("Surface: Failed to create surface from canvas");
        wgpuInstanceRelease(ctx->instance);
        delete ctx;
        return nullptr;
    }
    ydebug("Surface: Surface created from #canvas");

    return ctx;
}

void destroyWebGPUContext(WebGPUContext* ctx) {
    if (!ctx) return;

    if (ctx->surface) {
        wgpuSurfaceRelease(ctx->surface);
        ydebug("Surface: Surface released");
    }
    if (ctx->instance) {
        wgpuInstanceRelease(ctx->instance);
        ydebug("Surface: Instance released");
    }

    delete ctx;
}

WGPUInstance getInstance(WebGPUContext* ctx) {
    return ctx ? ctx->instance : nullptr;
}

WGPUSurface getSurface(WebGPUContext* ctx) {
    return ctx ? ctx->surface : nullptr;
}

} // namespace webasm
} // namespace platform
} // namespace yetty
