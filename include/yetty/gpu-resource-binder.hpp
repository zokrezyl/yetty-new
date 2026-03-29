#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <yetty/gpu-resource-set.hpp>
#include <yetty/yetty-gpu-context.hpp>

namespace yetty {

class GpuAllocator;

// GpuResourceBinder - creates GPU resources from GpuResourceSet descriptions,
// uploads CPU data, packs uniforms, creates bind groups.
class GpuResourceBinder : public core::FactoryObject<GpuResourceBinder> {
public:
    static Result<GpuResourceBinder*> createImpl(const YettyGpuContext& yettyGpuContext,
                                                  GpuAllocator* allocator);

    virtual ~GpuResourceBinder() = default;

    // Called every frame - creates GPU resources on first call, uploads data from pointers
    virtual void addGpuResourceSet(const GpuResourceSet& gpuResourceSet) = 0;

    // Called every frame after all addGpuResourceSet calls - binds to render pass
    virtual void bind(WGPURenderPassEncoder pass, uint32_t groupIndex) = 0;

protected:
    GpuResourceBinder() = default;
};

} // namespace yetty
