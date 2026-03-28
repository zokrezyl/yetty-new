// iOS surface.mm - WebGPU surface from CAMetalLayer

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#import <QuartzCore/CAMetalLayer.h>

WGPUSurface createSurface(CAMetalLayer* layer) {
    if (!layer) {
        yerror("createSurface: null layer");
        return nullptr;
    }

    // Create instance (required by wgpuInstanceCreateSurface)
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        yerror("createSurface: Failed to create WebGPU instance");
        return nullptr;
    }

    WGPUSurfaceDescriptorFromMetalLayer metalDesc = {};
    metalDesc.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
    metalDesc.layer = (__bridge void*)layer;

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&metalDesc);

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);

    // Release instance - surface retains what it needs internally
    wgpuInstanceRelease(instance);

    if (!surface) {
        yerror("createSurface: wgpuInstanceCreateSurface failed");
        return nullptr;
    }

    ydebug("createSurface: Surface created from CAMetalLayer");
    return surface;
}
