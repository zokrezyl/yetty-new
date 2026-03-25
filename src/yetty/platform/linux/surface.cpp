// Linux surface.cpp - WebGPU instance and surface creation
//
// Platform creates (on MAIN THREAD):
//   - WGPUInstance
//   - WGPUSurface (from instance + GLFW window)
//
// These are passed to Yetty, which creates adapter/device/queue on render thread.
//
// Platform owns instance and surface lifetime - destroys them after Yetty exits.

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>
#include <glfw3webgpu.h>

// Holds instance + surface created by platform
struct WebGPUContext {
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
};

WebGPUContext* createWebGPUContext(GLFWwindow* window) {
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

    // Create surface from GLFW window
    ctx->surface = glfwCreateWindowWGPUSurface(ctx->instance, window);
    if (!ctx->surface) {
        yerror("Surface: Failed to create surface from GLFW window");
        wgpuInstanceRelease(ctx->instance);
        delete ctx;
        return nullptr;
    }
    ydebug("Surface: Surface created");

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
