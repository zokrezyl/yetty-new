#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <webgpu/webgpu.h>

namespace yetty {

class MsMsdfFont;

/**
 * SharedBindGroup - Shared GPU resources for bind group 0.
 *
 * Contains large resources that should be shared across all views:
 * - MSDF font atlas texture
 * - MSDF font sampler
 * - MSDF font glyph metadata
 *
 * Does NOT contain uniforms - each view owns its own uniforms.
 * Does NOT use GpuAllocator - resources are small and don't need tracking.
 *
 * Ownership: Created by Yetty, stored in YettyContext, used by all views.
 */
class SharedBindGroup : public core::FactoryObject<SharedBindGroup> {
public:
    static Result<SharedBindGroup*> createImpl(WGPUDevice device);

    virtual ~SharedBindGroup() = default;

    // Get the bind group for use in rendering
    virtual WGPUBindGroup getBindGroup() const = 0;

    // Get the bind group layout (views need this to create compatible pipelines)
    virtual WGPUBindGroupLayout getBindGroupLayout() const = 0;

    // Set the MSDF font (updates bind group)
    virtual Result<void> setMsdfFont(MsMsdfFont* font) = 0;

protected:
    SharedBindGroup() = default;
};

} // namespace yetty
