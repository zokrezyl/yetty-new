#include <yetty/shared-bind-group.hpp>
#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>

namespace yetty {

class SharedBindGroupImpl : public SharedBindGroup {
public:
    explicit SharedBindGroupImpl(WGPUDevice device) : _device(device) {}

    ~SharedBindGroupImpl() override {
        if (_bindGroup) wgpuBindGroupRelease(_bindGroup);
        if (_bindGroupLayout) wgpuBindGroupLayoutRelease(_bindGroupLayout);
        if (_dummySampler) wgpuSamplerRelease(_dummySampler);
        if (_dummyTextureView) wgpuTextureViewRelease(_dummyTextureView);
        if (_dummyTexture) wgpuTextureRelease(_dummyTexture);
        if (_dummyBuffer) wgpuBufferRelease(_dummyBuffer);
    }

    const char* typeName() const override { return "SharedBindGroup"; }

    Result<void> init() {
        // Create bind group layout for shared resources (MSDF font only)
        // binding 0: font atlas texture
        // binding 1: font sampler
        // binding 2: font glyph metadata buffer
        WGPUBindGroupLayoutEntry entries[3] = {};

        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].texture.sampleType = WGPUTextureSampleType_Float;
        entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Fragment;
        entries[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

        WGPUBindGroupLayoutDescriptor layoutDesc = {};
        layoutDesc.label = WGPU_STR("shared bind group layout");
        layoutDesc.entryCount = 3;
        layoutDesc.entries = entries;
        _bindGroupLayout = wgpuDeviceCreateBindGroupLayout(_device, &layoutDesc);
        if (!_bindGroupLayout) {
            return Err<void>("Failed to create shared bind group layout");
        }

        // Create dummy resources for initial bind group (before font is set)
        if (auto res = createDummyResources(); !res) {
            return res;
        }

        // Create initial bind group with dummy resources
        if (auto res = createBindGroup(); !res) {
            return res;
        }

        ydebug("SharedBindGroup: created");
        return Ok();
    }

    WGPUBindGroup getBindGroup() const override { return _bindGroup; }
    WGPUBindGroupLayout getBindGroupLayout() const override { return _bindGroupLayout; }

    Result<void> setMsdfFont(MsMsdfFont* font) override {
        _font = font;
        // Recreate bind group with font resources
        // TODO: implement when MsMsdfFont interface is defined
        return Ok();
    }

private:
    Result<void> createDummyResources() {
        // Dummy texture (1x1 pixel, tiny)
        WGPUTextureDescriptor texDesc = {};
        texDesc.label = WGPU_STR("shared dummy texture");
        texDesc.size = {1, 1, 1};
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.usage = WGPUTextureUsage_TextureBinding;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        _dummyTexture = wgpuDeviceCreateTexture(_device, &texDesc);
        if (!_dummyTexture) {
            return Err<void>("Failed to create dummy texture");
        }

        WGPUTextureViewDescriptor viewDesc = {};
        viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
        viewDesc.dimension = WGPUTextureViewDimension_2D;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        _dummyTextureView = wgpuTextureCreateView(_dummyTexture, &viewDesc);

        // Dummy sampler
        WGPUSamplerDescriptor samplerDesc = {};
        samplerDesc.magFilter = WGPUFilterMode_Linear;
        samplerDesc.minFilter = WGPUFilterMode_Linear;
        samplerDesc.maxAnisotropy = 1;
        _dummySampler = wgpuDeviceCreateSampler(_device, &samplerDesc);

        // Dummy buffer (4 bytes, tiny)
        WGPUBufferDescriptor bufDesc = {};
        bufDesc.label = WGPU_STR("shared dummy buffer");
        bufDesc.size = 4;
        bufDesc.usage = WGPUBufferUsage_Storage;
        _dummyBuffer = wgpuDeviceCreateBuffer(_device, &bufDesc);
        if (!_dummyBuffer) {
            return Err<void>("Failed to create dummy buffer");
        }

        return Ok();
    }

    Result<void> createBindGroup() {
        if (_bindGroup) {
            wgpuBindGroupRelease(_bindGroup);
            _bindGroup = nullptr;
        }

        // Use font resources if available, otherwise dummy
        WGPUTextureView textureView = _dummyTextureView;
        WGPUSampler sampler = _dummySampler;
        WGPUBuffer metadataBuffer = _dummyBuffer;
        uint64_t metadataSize = 4;

        // TODO: if (_font) use font's resources instead

        WGPUBindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].textureView = textureView;
        entries[1].binding = 1;
        entries[1].sampler = sampler;
        entries[2].binding = 2;
        entries[2].buffer = metadataBuffer;
        entries[2].size = metadataSize;

        WGPUBindGroupDescriptor desc = {};
        desc.label = WGPU_STR("shared bind group");
        desc.layout = _bindGroupLayout;
        desc.entryCount = 3;
        desc.entries = entries;
        _bindGroup = wgpuDeviceCreateBindGroup(_device, &desc);
        if (!_bindGroup) {
            return Err<void>("Failed to create shared bind group");
        }

        return Ok();
    }

    WGPUDevice _device;
    MsMsdfFont* _font = nullptr;

    WGPUBindGroupLayout _bindGroupLayout = nullptr;
    WGPUBindGroup _bindGroup = nullptr;

    // Dummy resources (used when no font set)
    WGPUTexture _dummyTexture = nullptr;
    WGPUTextureView _dummyTextureView = nullptr;
    WGPUSampler _dummySampler = nullptr;
    WGPUBuffer _dummyBuffer = nullptr;
};

Result<SharedBindGroup*> SharedBindGroup::createImpl(WGPUDevice device) {
    if (!device) {
        return Err<SharedBindGroup*>("SharedBindGroup: device is null");
    }

    auto* impl = new SharedBindGroupImpl(device);
    if (auto res = impl->init(); !res) {
        delete impl;
        return Err<SharedBindGroup*>("SharedBindGroup init failed");
    }
    return Ok(static_cast<SharedBindGroup*>(impl));
}

} // namespace yetty
