/* iOS surface - WebGPU surface from CAMetalLayer */

#include <webgpu/webgpu.h>
#import <QuartzCore/CAMetalLayer.h>

WGPUSurface yetty_platform_create_surface_from_layer(WGPUInstance instance, CAMetalLayer *layer)
{
    if (!instance || !layer)
        return NULL;

    WGPUSurfaceDescriptorFromMetalLayer metal_desc = {0};
    metal_desc.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
    metal_desc.layer = (__bridge void *)layer;

    WGPUSurfaceDescriptor surface_desc = {0};
    surface_desc.nextInChain = (WGPUChainedStruct *)&metal_desc;

    return wgpuInstanceCreateSurface(instance, &surface_desc);
}
