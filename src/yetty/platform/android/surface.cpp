// Android surface.cpp - WebGPU surface from ANativeWindow

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#include <android/native_window.h>

WGPUSurface createSurface(WGPUInstance instance, ANativeWindow* window) {
    if (!instance || !window) {
        yerror("createSurface: null instance or window");
        return nullptr;
    }

    WGPUSurfaceDescriptorFromAndroidNativeWindow androidDesc = {};
    androidDesc.chain.sType = WGPUSType_SurfaceDescriptorFromAndroidNativeWindow;
    androidDesc.window = window;

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&androidDesc);

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);
    if (!surface) {
        yerror("createSurface: wgpuInstanceCreateSurface failed");
        return nullptr;
    }

    ydebug("createSurface: Surface created from ANativeWindow");
    return surface;
}
