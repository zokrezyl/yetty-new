// Android surface.cpp - WebGPU surface from ANativeWindow

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#include <android/native_window.h>

WGPUSurface createSurface(ANativeWindow* window) {
    if (!window) {
        yerror("createSurface: null window");
        return nullptr;
    }

    // Create instance (required by wgpuInstanceCreateSurface)
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        yerror("createSurface: Failed to create WebGPU instance");
        return nullptr;
    }

    WGPUSurfaceDescriptorFromAndroidNativeWindow androidDesc = {};
    androidDesc.chain.sType = WGPUSType_SurfaceDescriptorFromAndroidNativeWindow;
    androidDesc.window = window;

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&androidDesc);

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);

    // Release instance - surface retains what it needs internally
    wgpuInstanceRelease(instance);

    if (!surface) {
        yerror("createSurface: wgpuInstanceCreateSurface failed");
        return nullptr;
    }

    ydebug("createSurface: Surface created from ANativeWindow");
    return surface;
}
