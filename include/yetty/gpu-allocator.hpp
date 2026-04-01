#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/wgpu-compat.hpp>
#include <webgpu/webgpu.h>
#include <cstdint>
#include <string>
#include <vector>

namespace yetty {

class GpuAllocator : public core::FactoryObject<GpuAllocator> {
public:
    static Result<GpuAllocator *> createImpl(WGPUDevice device);

    virtual ~GpuAllocator() = default;

    // Buffer allocation — name extracted from desc.label
    virtual WGPUBuffer createBuffer(const WGPUBufferDescriptor& desc) = 0;
    virtual void releaseBuffer(WGPUBuffer buffer) = 0;

    // Texture allocation — name extracted from desc.label
    virtual WGPUTexture createTexture(const WGPUTextureDescriptor& desc) = 0;
    virtual void releaseTexture(WGPUTexture texture) = 0;

    // Query
    virtual uint64_t totalAllocatedBytes() const = 0;
    virtual uint64_t totalBufferBytes() const = 0;
    virtual uint64_t totalTextureBytes() const = 0;
    virtual uint32_t allocationCount() const = 0;

    // Log all live allocations with names and sizes
    virtual void dumpAllocations() const = 0;

    // Return all live allocations as formatted text
    virtual std::string dumpAllocationsToString() const = 0;

protected:
    GpuAllocator() = default;
};

} // namespace yetty
