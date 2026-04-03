// Android surface.cpp - WebGPU surface from ANativeWindow

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#include <android/native_window.h>

WGPUSurface createSurface(WGPUInstance instance, ANativeWindow* window) {
    if (!instance || !window) {
        yerror("createSurface: null instance or window");
        return nullptr;
    }

    WGPUSurfaceSourceAndroidNativeWindow androidSource = {};
    androidSource.chain.sType = WGPUSType_SurfaceSourceAndroidNativeWindow;
    androidSource.window = window;

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = &androidSource.chain;

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);
    if (!surface) {
        yerror("createSurface: wgpuInstanceCreateSurface failed");
        return nullptr;
    }

    ydebug("createSurface: Surface created from ANativeWindow");
    return surface;
}
