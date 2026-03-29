#include <yetty/gpu-resource-binder.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>

#include <unordered_map>
#include <vector>

namespace yetty {

//=============================================================================
// GpuResourceBinderImpl
//=============================================================================

class GpuResourceBinderImpl : public GpuResourceBinder {
public:
    GpuResourceBinderImpl(const YettyGpuContext& yettyGpuContext, GpuAllocator* allocator)
        : _yettyGpuContext(yettyGpuContext), _allocator(allocator) {}

    ~GpuResourceBinderImpl() override {
        cleanup();
    }

    Result<void> init() {
        return Ok();
    }

    //=========================================================================
    // addGpuResourceSet - called every frame
    //=========================================================================

    void addGpuResourceSet(const GpuResourceSet& gpuResourceSet) override {
        auto it = _entries.find(gpuResourceSet.name);

        if (it == _entries.end()) {
            // First time - create GPU resources
            createResources(gpuResourceSet);
            it = _entries.find(gpuResourceSet.name);
        }

        // Upload data from pointers
        uploadData(it->second, gpuResourceSet);
    }

    //=========================================================================
    // bind - called every frame after addGpuResourceSet calls
    //=========================================================================

    void bind(WGPURenderPassEncoder pass, uint32_t groupIndex) override {
        if (!_bindGroup) {
            createBindGroup();
        }

        if (_bindGroup) {
            wgpuRenderPassEncoderSetBindGroup(pass, groupIndex, _bindGroup, 0, nullptr);
        }
    }

private:
    //=========================================================================
    // Internal types
    //=========================================================================

    struct ResourceEntry {
        bool shared = false;

        WGPUTexture texture = nullptr;
        WGPUTextureView textureView = nullptr;
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;
        WGPUTextureFormat textureFormat = WGPUTextureFormat_Undefined;

        WGPUSampler sampler = nullptr;

        WGPUBuffer buffer = nullptr;
        size_t bufferSize = 0;
        bool bufferReadonly = true;
    };

    //=========================================================================
    // Create GPU resources (first time only)
    //=========================================================================

    void createResources(const GpuResourceSet& gpuResourceSet) {
        ResourceEntry entry;
        entry.shared = gpuResourceSet.shared;

        // Create texture if described
        if (gpuResourceSet.textureWidth > 0 && gpuResourceSet.textureHeight > 0) {
            WGPUTextureDescriptor texDesc = {};
            texDesc.label = WGPU_STR(gpuResourceSet.name.c_str());
            texDesc.size = {gpuResourceSet.textureWidth, gpuResourceSet.textureHeight, 1};
            texDesc.mipLevelCount = 1;
            texDesc.sampleCount = 1;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.format = gpuResourceSet.textureFormat;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

            entry.texture = _allocator->createTexture(texDesc);
            entry.textureWidth = gpuResourceSet.textureWidth;
            entry.textureHeight = gpuResourceSet.textureHeight;
            entry.textureFormat = gpuResourceSet.textureFormat;

            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = gpuResourceSet.textureFormat;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.mipLevelCount = 1;
            viewDesc.arrayLayerCount = 1;
            entry.textureView = wgpuTextureCreateView(entry.texture, &viewDesc);

            // Create sampler
            WGPUSamplerDescriptor samplerDesc = {};
            samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
            samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
            samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
            samplerDesc.magFilter = gpuResourceSet.samplerFilter;
            samplerDesc.minFilter = gpuResourceSet.samplerFilter;
            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            samplerDesc.maxAnisotropy = 1;
            entry.sampler = wgpuDeviceCreateSampler(_yettyGpuContext.device, &samplerDesc);

            ydebug("GpuResourceBinder: created texture {}x{} for '{}'",
                   entry.textureWidth, entry.textureHeight, gpuResourceSet.name);
        }

        // Create buffer if described
        if (gpuResourceSet.bufferSize > 0) {
            WGPUBufferDescriptor bufDesc = {};
            bufDesc.label = WGPU_STR(gpuResourceSet.name.c_str());
            bufDesc.size = gpuResourceSet.bufferSize;
            bufDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
            entry.buffer = _allocator->createBuffer(bufDesc);
            entry.bufferSize = gpuResourceSet.bufferSize;
            entry.bufferReadonly = gpuResourceSet.bufferReadonly;

            ydebug("GpuResourceBinder: created buffer {} bytes for '{}'",
                   entry.bufferSize, gpuResourceSet.name);
        }

        _entries[gpuResourceSet.name] = std::move(entry);
        _bindGroupDirty = true;
    }

    //=========================================================================
    // Upload data from pointers
    //=========================================================================

    void uploadData(ResourceEntry& entry, const GpuResourceSet& gpuResourceSet) {
        // Upload texture data
        if (entry.texture && gpuResourceSet.textureData && gpuResourceSet.textureDataSize > 0) {
            WGPUTexelCopyTextureInfo destInfo = {};
            destInfo.texture = entry.texture;
            destInfo.mipLevel = 0;
            destInfo.origin = {0, 0, 0};
            destInfo.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout srcLayout = {};
            srcLayout.offset = 0;
            srcLayout.bytesPerRow = entry.textureWidth;  // Assumes R8
            srcLayout.rowsPerImage = entry.textureHeight;

            WGPUExtent3D extent = {entry.textureWidth, entry.textureHeight, 1};
            wgpuQueueWriteTexture(_yettyGpuContext.queue, &destInfo, gpuResourceSet.textureData,
                                  gpuResourceSet.textureDataSize, &srcLayout, &extent);
        }

        // Upload buffer data
        if (entry.buffer && gpuResourceSet.bufferData && gpuResourceSet.bufferDataSize > 0) {
            wgpuQueueWriteBuffer(_yettyGpuContext.queue, entry.buffer, 0,
                                 gpuResourceSet.bufferData, gpuResourceSet.bufferDataSize);
        }
    }

    //=========================================================================
    // Create bind group
    //=========================================================================

    void createBindGroup() {
        if (_entries.empty()) return;

        std::vector<WGPUBindGroupLayoutEntry> layoutEntries;
        std::vector<WGPUBindGroupEntry> groupEntries;
        uint32_t binding = 0;

        for (auto& [name, entry] : _entries) {
            if (entry.textureView) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = binding;
                layoutEntry.visibility = WGPUShaderStage_Fragment;
                layoutEntry.texture.sampleType = WGPUTextureSampleType_Float;
                layoutEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = binding;
                groupEntry.textureView = entry.textureView;
                groupEntries.push_back(groupEntry);
                binding++;
            }

            if (entry.sampler) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = binding;
                layoutEntry.visibility = WGPUShaderStage_Fragment;
                layoutEntry.sampler.type = WGPUSamplerBindingType_Filtering;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = binding;
                groupEntry.sampler = entry.sampler;
                groupEntries.push_back(groupEntry);
                binding++;
            }

            if (entry.buffer) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = binding;
                layoutEntry.visibility = WGPUShaderStage_Fragment;
                layoutEntry.buffer.type = entry.bufferReadonly
                    ? WGPUBufferBindingType_ReadOnlyStorage
                    : WGPUBufferBindingType_Storage;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = binding;
                groupEntry.buffer = entry.buffer;
                groupEntry.size = entry.bufferSize;
                groupEntries.push_back(groupEntry);
                binding++;
            }
        }

        if (layoutEntries.empty()) return;

        // Create layout
        WGPUBindGroupLayoutDescriptor layoutDesc = {};
        layoutDesc.entryCount = layoutEntries.size();
        layoutDesc.entries = layoutEntries.data();
        _bindGroupLayout = wgpuDeviceCreateBindGroupLayout(_yettyGpuContext.device, &layoutDesc);

        // Create bind group
        WGPUBindGroupDescriptor groupDesc = {};
        groupDesc.layout = _bindGroupLayout;
        groupDesc.entryCount = groupEntries.size();
        groupDesc.entries = groupEntries.data();
        _bindGroup = wgpuDeviceCreateBindGroup(_yettyGpuContext.device, &groupDesc);

        _bindGroupDirty = false;
        ydebug("GpuResourceBinder: created bind group with {} bindings", binding);
    }

    //=========================================================================
    // Cleanup
    //=========================================================================

    void cleanup() {
        if (_bindGroup) {
            wgpuBindGroupRelease(_bindGroup);
            _bindGroup = nullptr;
        }
        if (_bindGroupLayout) {
            wgpuBindGroupLayoutRelease(_bindGroupLayout);
            _bindGroupLayout = nullptr;
        }
        for (auto& [name, entry] : _entries) {
            if (entry.buffer) _allocator->releaseBuffer(entry.buffer);
            if (entry.sampler) wgpuSamplerRelease(entry.sampler);
            if (entry.textureView) wgpuTextureViewRelease(entry.textureView);
            if (entry.texture) _allocator->releaseTexture(entry.texture);
        }
        _entries.clear();
    }

    //=========================================================================
    // Data
    //=========================================================================

    YettyGpuContext _yettyGpuContext;
    GpuAllocator* _allocator;

    std::unordered_map<std::string, ResourceEntry> _entries;

    WGPUBindGroupLayout _bindGroupLayout = nullptr;
    WGPUBindGroup _bindGroup = nullptr;
    bool _bindGroupDirty = true;
};

//=============================================================================
// Factory
//=============================================================================

Result<GpuResourceBinder*> GpuResourceBinder::createImpl(const YettyGpuContext& yettyGpuContext,
                                                          GpuAllocator* allocator) {
    auto* binder = new GpuResourceBinderImpl(yettyGpuContext, allocator);
    if (auto res = binder->init(); !res) {
        delete binder;
        return Err<GpuResourceBinder*>("Failed to init GpuResourceBinder", res);
    }
    return Ok(binder);
}

} // namespace yetty
