// iOS surface.mm - WebGPU surface from CAMetalLayer

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#import <QuartzCore/CAMetalLayer.h>

WGPUSurface createSurface(WGPUInstance instance, CAMetalLayer* layer) {
    if (!instance || !layer) {
        yerror("createSurface: null instance or layer");
        return nullptr;
    }

    WGPUSurfaceDescriptorFromMetalLayer metalDesc = {};
    metalDesc.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
    metalDesc.layer = (__bridge void*)layer;

    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&metalDesc);

    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);
    if (!surface) {
        yerror("createSurface: wgpuInstanceCreateSurface failed");
        return nullptr;
    }

    ydebug("createSurface: Surface created from CAMetalLayer");
    return surface;
}
