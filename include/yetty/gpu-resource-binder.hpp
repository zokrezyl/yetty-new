#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <yetty/gpu-resource-set.hpp>
#include <yetty/yetty-gpu-context.hpp>

namespace yetty {

class GpuAllocator;
class ShaderManager;

/**
 * GpuResourceBinder - handles ALL GPU operations:
 * - Creates GPU resources from GpuResourceSet descriptions
 * - Generates WGSL binding code and passes to ShaderManager
 * - Compiles shaders from merged WGSL
 * - Creates bind group layouts, bind groups, pipeline
 * - Uploads CPU data, binds to render pass
 */
class GpuResourceBinder : public core::FactoryObject<GpuResourceBinder> {
public:
    static Result<GpuResourceBinder*> createImpl(
        const YettyGpuContext& yettyGpuContext,
        GpuAllocator* gpuAllocator,
        ShaderManager* shaderManager);

    virtual ~GpuResourceBinder() = default;

    /**
     * Submit resource set. Call each frame for every resource set.
     * First call: creates GPU resources (buffers, textures, samplers).
     * Subsequent calls: uploads new data from CPU pointers.
     * Identified by name - same name updates existing, new name creates new.
     */
    virtual Result<void> submitGpuResourceSet(const GpuResourceSet& gpuResourceSet) = 0;

    // Finalize: generate binding code, merge shaders, compile, create pipeline
    virtual Result<void> finalize() = 0;

    // Bind to render pass
    virtual Result<void> bind(WGPURenderPassEncoder pass, uint32_t groupIndex) = 0;

    // Get pipeline (valid after finalize)
    virtual WGPURenderPipeline getPipeline() const = 0;

    // Get quad vertex buffer (valid after finalize)
    virtual WGPUBuffer getQuadVertexBuffer() const = 0;

protected:
    GpuResourceBinder() = default;
};

} // namespace yetty
