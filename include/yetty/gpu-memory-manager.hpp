#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/gpu-buffer-manager.hpp>
#include <yetty/gpu-texture-manager.hpp>
#include <yetty/gpu-resource-set.hpp>

namespace yetty {

// Metadata config
struct MetadataConfig {
    uint32_t pool32Count = 0;
    uint32_t pool64Count = 1024;
    uint32_t pool128Count = 16;
    uint32_t pool256Count = 8;
};

/**
 * GpuMemoryManager - CPU-side memory management for GPU resources.
 *
 * Coordinates CardBufferManager and GpuTextureManager.
 * Returns GpuResourceSet for GpuResourceBinder to create actual GPU resources.
 */
class GpuMemoryManager : public core::FactoryObject<GpuMemoryManager> {
public:
    struct Config {
        MetadataConfig metadata;
        GpuTextureConfig texture;
    };

    static Result<GpuMemoryManager*> createImpl(Config config = {});

    virtual ~GpuMemoryManager() = default;

    // =========================================================================
    // Metadata operations (owned by GpuMemoryManager)
    // =========================================================================
    virtual Result<MetadataHandle> allocateMetadata(uint32_t size) = 0;
    virtual Result<void> deallocateMetadata(MetadataHandle handle) = 0;
    virtual Result<void> writeMetadata(MetadataHandle handle, const void* data, uint32_t size) = 0;
    virtual Result<void> writeMetadataAt(MetadataHandle handle, uint32_t offset, const void* data, uint32_t size) = 0;

    // =========================================================================
    // Manager accessors
    // =========================================================================
    virtual CardBufferManager* bufferManager() const = 0;
    virtual GpuTextureManager* textureManager() const = 0;

    // =========================================================================
    // GpuResourceSet output (for GpuResourceBinder)
    // =========================================================================
    virtual GpuResourceSet getGpuResourceSet() const = 0;

protected:
    GpuMemoryManager() = default;
};

}  // namespace yetty
