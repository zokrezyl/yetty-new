#include <yetty/gpu-resource-binder.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/shader-manager.hpp>
#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>

#include <vector>

namespace yetty {

//=============================================================================
// GpuResourceBinderImpl
//=============================================================================

class GpuResourceBinderImpl : public GpuResourceBinder {
public:
    GpuResourceBinderImpl(
        const YettyGpuContext& yettyGpuContext,
        GpuAllocator* gpuAllocator,
        ShaderManager* shaderManager)
        : _yettyGpuContext(yettyGpuContext),
          _gpuAllocator(gpuAllocator),
          _shaderManager(shaderManager) {}

    ~GpuResourceBinderImpl() override {
        cleanup();
    }

    Result<void> init() {
        if (!_gpuAllocator) {
            return Err<void>("GpuResourceBinder: null gpuAllocator");
        }
        if (!_shaderManager) {
            return Err<void>("GpuResourceBinder: null shaderManager");
        }
        if (!_yettyGpuContext.device) {
            return Err<void>("GpuResourceBinder: null device");
        }
        return Ok();
    }

    //=========================================================================
    // submitGpuResourceSet
    //=========================================================================

    Result<void> submitGpuResourceSet(const GpuResourceSet& gpuResourceSet) override {
        // Find existing by name
        for (size_t i = 0; i < _resourceSets.size(); i++) {
            if (_resourceSets[i].name == gpuResourceSet.name) {
                // Check if storage buffer needs resize
                if (gpuResourceSet.bufferSize > 0 &&
                    gpuResourceSet.bufferSize != _entries[i].storageBufferSize) {
                    // Buffer size changed - recreate buffer
                    if (_entries[i].storageBuffer) {
                        wgpuBufferRelease(_entries[i].storageBuffer);
                        _entries[i].storageBuffer = nullptr;
                    }
                    std::string storageLabel = gpuResourceSet.name + "_storage";
                    WGPUBufferDescriptor bufferDescriptor = {};
                    bufferDescriptor.label = WGPU_STR(storageLabel.c_str());
                    bufferDescriptor.size = gpuResourceSet.bufferSize;
                    bufferDescriptor.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
                    _entries[i].storageBuffer = _gpuAllocator->createBuffer(bufferDescriptor);
                    _entries[i].storageBufferSize = gpuResourceSet.bufferSize;
                    // Recreate bind group since buffer changed
                    if (_bindGroup) {
                        wgpuBindGroupRelease(_bindGroup);
                        _bindGroup = nullptr;
                    }
                    if (auto result = createBindGroup(); !result) {
                        return Err<void>("Failed to recreate bind group after buffer resize", result);
                    }
                    ydebug("GpuResourceBinder: resized storage buffer '{}' to {} bytes, recreated bind group", gpuResourceSet.name, gpuResourceSet.bufferSize);
                }
                _resourceSets[i] = gpuResourceSet;
                return uploadData(i);
            }
        }

        // New resource set
        _resourceSets.push_back(gpuResourceSet);
        _entries.push_back(ResourceEntry{});
        size_t index = _resourceSets.size() - 1;

        if (auto result = createResources(index); !result) {
            return Err<void>("Failed to create resources for: " + gpuResourceSet.name, result);
        }

        if (auto result = uploadData(index); !result) {
            return Err<void>("Failed to upload data for: " + gpuResourceSet.name, result);
        }

        return Ok();
    }

    //=========================================================================
    // finalize
    //=========================================================================

    Result<void> finalize() override {
        if (_finalized) {
            return Ok();
        }

        if (auto result = createBindGroup(); !result) {
            return Err<void>("Failed to create bind group", result);
        }

        std::string bindingCode = generateWgslBindings(0);
        _shaderManager->setBindingCode(bindingCode);

        Result<std::string> mergeResult = _shaderManager->merge();
        if (!mergeResult) {
            return Err<void>("Failed to merge shaders", mergeResult);
        }

        if (auto result = compileAndCreatePipeline(*mergeResult); !result) {
            return Err<void>("Failed to compile/create pipeline", result);
        }

        _finalized = true;
        return Ok();
    }

    //=========================================================================
    // bind
    //=========================================================================

    Result<void> bind(WGPURenderPassEncoder pass, uint32_t groupIndex) override {
        if (!_finalized) {
            return Err<void>("Binder not finalized");
        }
        if (!_bindGroup) {
            return Err<void>("Bind group is null");
        }
        wgpuRenderPassEncoderSetBindGroup(pass, groupIndex, _bindGroup, 0, nullptr);
        return Ok();
    }

    WGPURenderPipeline getPipeline() const override {
        return _pipeline;
    }

    WGPUBuffer getQuadVertexBuffer() const override {
        return _quadVertexBuffer;
    }

private:
    struct ResourceEntry {
        WGPUBuffer uniformBuffer = nullptr;
        size_t uniformSize = 0;

        WGPUTexture texture = nullptr;
        WGPUTextureView textureView = nullptr;
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;
        WGPUTextureFormat textureFormat = WGPUTextureFormat_Undefined;

        WGPUSampler sampler = nullptr;

        WGPUBuffer storageBuffer = nullptr;
        size_t storageBufferSize = 0;
        bool storageBufferReadonly = true;
    };

    //=========================================================================
    // generateWgslBindings
    //=========================================================================

    std::string generateWgslBindings(uint32_t groupIndex) const {
        std::string result;
        uint32_t bindingIndex = 0;

        for (const auto& resourceSet : _resourceSets) {
            if (resourceSet.uniformSize > 0 && !resourceSet.uniformWgslType.empty()) {
                std::string uniformName = resourceSet.uniformName.empty() ? resourceSet.name : resourceSet.uniformName;
                result += "@group(" + std::to_string(groupIndex) + ") @binding(" + std::to_string(bindingIndex++) + ") var<uniform> " + uniformName + ": " + resourceSet.uniformWgslType + ";\n";
            }

            if (resourceSet.textureWidth > 0 && resourceSet.textureHeight > 0) {
                std::string textureType = resourceSet.textureWgslType.empty() ? "texture_2d<f32>" : resourceSet.textureWgslType;
                std::string textureName = resourceSet.textureName.empty() ? (resourceSet.name + "Texture") : resourceSet.textureName;
                std::string samplerName = resourceSet.samplerName.empty() ? (resourceSet.name + "Sampler") : resourceSet.samplerName;
                result += "@group(" + std::to_string(groupIndex) + ") @binding(" + std::to_string(bindingIndex++) + ") var " + textureName + ": " + textureType + ";\n";
                result += "@group(" + std::to_string(groupIndex) + ") @binding(" + std::to_string(bindingIndex++) + ") var " + samplerName + ": sampler;\n";
            }

            if (resourceSet.bufferSize > 0) {
                std::string bufferType = resourceSet.bufferWgslType.empty() ? "array<u32>" : resourceSet.bufferWgslType;
                std::string bufferName = resourceSet.bufferName.empty() ? (resourceSet.name + "Buffer") : resourceSet.bufferName;
                std::string accessMode = resourceSet.bufferReadonly ? "read" : "read_write";
                result += "@group(" + std::to_string(groupIndex) + ") @binding(" + std::to_string(bindingIndex++) + ") var<storage, " + accessMode + "> " + bufferName + ": " + bufferType + ";\n";
            }
        }

        return result;
    }

    //=========================================================================
    // createResources
    //=========================================================================

    Result<void> createResources(size_t index) {
        const GpuResourceSet& resourceSet = _resourceSets[index];
        ResourceEntry& resourceEntry = _entries[index];

        if (resourceSet.uniformSize > 0) {
            std::string uniformLabel = resourceSet.name + "_uniform";
            WGPUBufferDescriptor bufferDescriptor = {};
            bufferDescriptor.label = WGPU_STR(uniformLabel.c_str());
            bufferDescriptor.size = resourceSet.uniformSize;
            bufferDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            resourceEntry.uniformBuffer = _gpuAllocator->createBuffer(bufferDescriptor);
            if (!resourceEntry.uniformBuffer) {
                return Err<void>("Failed to create uniform buffer for: " + resourceSet.name);
            }
            resourceEntry.uniformSize = resourceSet.uniformSize;
        }

        if (resourceSet.textureWidth > 0 && resourceSet.textureHeight > 0) {
            WGPUTextureDescriptor textureDescriptor = {};
            textureDescriptor.label = WGPU_STR(resourceSet.name.c_str());
            textureDescriptor.size = {resourceSet.textureWidth, resourceSet.textureHeight, 1};
            textureDescriptor.mipLevelCount = 1;
            textureDescriptor.sampleCount = 1;
            textureDescriptor.dimension = WGPUTextureDimension_2D;
            textureDescriptor.format = resourceSet.textureFormat;
            textureDescriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

            resourceEntry.texture = _gpuAllocator->createTexture(textureDescriptor);
            if (!resourceEntry.texture) {
                return Err<void>("Failed to create texture for: " + resourceSet.name);
            }
            resourceEntry.textureWidth = resourceSet.textureWidth;
            resourceEntry.textureHeight = resourceSet.textureHeight;
            resourceEntry.textureFormat = resourceSet.textureFormat;

            WGPUTextureViewDescriptor textureViewDescriptor = {};
            textureViewDescriptor.format = resourceSet.textureFormat;
            textureViewDescriptor.dimension = WGPUTextureViewDimension_2D;
            textureViewDescriptor.mipLevelCount = 1;
            textureViewDescriptor.arrayLayerCount = 1;
            resourceEntry.textureView = wgpuTextureCreateView(resourceEntry.texture, &textureViewDescriptor);

            WGPUSamplerDescriptor samplerDescriptor = {};
            samplerDescriptor.addressModeU = WGPUAddressMode_ClampToEdge;
            samplerDescriptor.addressModeV = WGPUAddressMode_ClampToEdge;
            samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
            samplerDescriptor.magFilter = resourceSet.samplerFilter;
            samplerDescriptor.minFilter = resourceSet.samplerFilter;
            samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            samplerDescriptor.maxAnisotropy = 1;
            resourceEntry.sampler = wgpuDeviceCreateSampler(_yettyGpuContext.device, &samplerDescriptor);
        }

        if (resourceSet.bufferSize > 0) {
            std::string storageLabel = resourceSet.name + "_storage";
            WGPUBufferDescriptor bufferDescriptor = {};
            bufferDescriptor.label = WGPU_STR(storageLabel.c_str());
            bufferDescriptor.size = resourceSet.bufferSize;
            bufferDescriptor.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
            resourceEntry.storageBuffer = _gpuAllocator->createBuffer(bufferDescriptor);
            if (!resourceEntry.storageBuffer) {
                return Err<void>("Failed to create storage buffer for: " + resourceSet.name);
            }
            resourceEntry.storageBufferSize = resourceSet.bufferSize;
            resourceEntry.storageBufferReadonly = resourceSet.bufferReadonly;
        }

        return Ok();
    }

    //=========================================================================
    // uploadData
    //=========================================================================

    Result<void> uploadData(size_t index) {
        const GpuResourceSet& resourceSet = _resourceSets[index];
        const ResourceEntry& resourceEntry = _entries[index];

        if (resourceEntry.uniformBuffer && resourceSet.uniformData && resourceSet.uniformDataSize > 0) {
            wgpuQueueWriteBuffer(_yettyGpuContext.queue, resourceEntry.uniformBuffer, 0,
                                 resourceSet.uniformData, resourceSet.uniformDataSize);
        }

        if (resourceEntry.texture && resourceSet.textureData && resourceSet.textureDataSize > 0) {
            WGPUTexelCopyTextureInfo destinationInfo = {};
            destinationInfo.texture = resourceEntry.texture;
            destinationInfo.mipLevel = 0;
            destinationInfo.origin = {0, 0, 0};
            destinationInfo.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout sourceLayout = {};
            sourceLayout.offset = 0;
            sourceLayout.bytesPerRow = resourceEntry.textureWidth;
            sourceLayout.rowsPerImage = resourceEntry.textureHeight;

            WGPUExtent3D extent = {resourceEntry.textureWidth, resourceEntry.textureHeight, 1};
            wgpuQueueWriteTexture(_yettyGpuContext.queue, &destinationInfo,
                                  resourceSet.textureData, resourceSet.textureDataSize,
                                  &sourceLayout, &extent);
        }

        if (resourceEntry.storageBuffer && resourceSet.bufferData && resourceSet.bufferDataSize > 0) {
            wgpuQueueWriteBuffer(_yettyGpuContext.queue, resourceEntry.storageBuffer, 0,
                                 resourceSet.bufferData, resourceSet.bufferDataSize);
        }

        return Ok();
    }

    //=========================================================================
    // createBindGroup
    //=========================================================================

    Result<void> createBindGroup() {
        if (_entries.empty()) {
            return Err<void>("No resource entries");
        }

        std::vector<WGPUBindGroupLayoutEntry> layoutEntries;
        std::vector<WGPUBindGroupEntry> groupEntries;
        uint32_t bindingIndex = 0;

        for (const ResourceEntry& resourceEntry : _entries) {
            if (resourceEntry.uniformBuffer) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = bindingIndex;
                layoutEntry.visibility = WGPUShaderStage_Fragment | WGPUShaderStage_Vertex;
                layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = bindingIndex;
                groupEntry.buffer = resourceEntry.uniformBuffer;
                groupEntry.size = resourceEntry.uniformSize;
                groupEntries.push_back(groupEntry);
                bindingIndex++;
            }

            if (resourceEntry.textureView) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = bindingIndex;
                layoutEntry.visibility = WGPUShaderStage_Fragment;
                layoutEntry.texture.sampleType = WGPUTextureSampleType_Float;
                layoutEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = bindingIndex;
                groupEntry.textureView = resourceEntry.textureView;
                groupEntries.push_back(groupEntry);
                bindingIndex++;
            }

            if (resourceEntry.sampler) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = bindingIndex;
                layoutEntry.visibility = WGPUShaderStage_Fragment;
                layoutEntry.sampler.type = WGPUSamplerBindingType_Filtering;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = bindingIndex;
                groupEntry.sampler = resourceEntry.sampler;
                groupEntries.push_back(groupEntry);
                bindingIndex++;
            }

            if (resourceEntry.storageBuffer) {
                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = bindingIndex;
                layoutEntry.visibility = WGPUShaderStage_Fragment;
                layoutEntry.buffer.type = resourceEntry.storageBufferReadonly
                    ? WGPUBufferBindingType_ReadOnlyStorage
                    : WGPUBufferBindingType_Storage;
                layoutEntries.push_back(layoutEntry);

                WGPUBindGroupEntry groupEntry = {};
                groupEntry.binding = bindingIndex;
                groupEntry.buffer = resourceEntry.storageBuffer;
                groupEntry.size = resourceEntry.storageBufferSize;
                groupEntries.push_back(groupEntry);
                bindingIndex++;
            }
        }

        if (layoutEntries.empty()) {
            return Err<void>("No bindings");
        }

        WGPUBindGroupLayoutDescriptor layoutDescriptor = {};
        layoutDescriptor.entryCount = layoutEntries.size();
        layoutDescriptor.entries = layoutEntries.data();
        _bindGroupLayout = wgpuDeviceCreateBindGroupLayout(_yettyGpuContext.device, &layoutDescriptor);
        if (!_bindGroupLayout) {
            return Err<void>("Failed to create bind group layout");
        }

        WGPUBindGroupDescriptor groupDescriptor = {};
        groupDescriptor.layout = _bindGroupLayout;
        groupDescriptor.entryCount = groupEntries.size();
        groupDescriptor.entries = groupEntries.data();
        _bindGroup = wgpuDeviceCreateBindGroup(_yettyGpuContext.device, &groupDescriptor);
        if (!_bindGroup) {
            return Err<void>("Failed to create bind group");
        }

        return Ok();
    }

    //=========================================================================
    // compileAndCreatePipeline
    //=========================================================================

    Result<void> compileAndCreatePipeline(const std::string& wgslCode) {
        WGPUShaderSourceWGSL wgslSourceDescriptor = {};
        wgslSourceDescriptor.chain.sType = WGPUSType_ShaderSourceWGSL;
        WGPU_SHADER_CODE(wgslSourceDescriptor, wgslCode);

        WGPUShaderModuleDescriptor shaderModuleDescriptor = {};
        shaderModuleDescriptor.label = WGPU_STR("terminal shader");
        shaderModuleDescriptor.nextInChain = &wgslSourceDescriptor.chain;

        _shaderModule = wgpuDeviceCreateShaderModule(_yettyGpuContext.device, &shaderModuleDescriptor);
        if (!_shaderModule) {
            return Err<void>("Failed to compile shader");
        }

        // Quad vertex buffer
        float quadVertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
            -1.0f,  1.0f,
             1.0f, -1.0f,
             1.0f,  1.0f,
        };
        WGPUBufferDescriptor quadBufferDescriptor = {};
        quadBufferDescriptor.label = WGPU_STR("quad vertices");
        quadBufferDescriptor.size = sizeof(quadVertices);
        quadBufferDescriptor.usage = WGPUBufferUsage_Vertex;
        quadBufferDescriptor.mappedAtCreation = true;
        _quadVertexBuffer = _gpuAllocator->createBuffer(quadBufferDescriptor);
        if (!_quadVertexBuffer) {
            return Err<void>("Failed to create quad vertex buffer");
        }
        void* mappedData = wgpuBufferGetMappedRange(_quadVertexBuffer, 0, sizeof(quadVertices));
        memcpy(mappedData, quadVertices, sizeof(quadVertices));
        wgpuBufferUnmap(_quadVertexBuffer);

        // Pipeline layout
        WGPUBindGroupLayout bindGroupLayouts[1] = { _bindGroupLayout };
        WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = {};
        pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
        pipelineLayoutDescriptor.bindGroupLayouts = bindGroupLayouts;
        _pipelineLayout = wgpuDeviceCreatePipelineLayout(_yettyGpuContext.device, &pipelineLayoutDescriptor);
        if (!_pipelineLayout) {
            return Err<void>("Failed to create pipeline layout");
        }

        // Pipeline
        WGPUVertexAttribute positionAttribute = {};
        positionAttribute.format = WGPUVertexFormat_Float32x2;
        positionAttribute.offset = 0;
        positionAttribute.shaderLocation = 0;

        WGPUVertexBufferLayout vertexBufferLayout = {};
        vertexBufferLayout.arrayStride = 2 * sizeof(float);
        vertexBufferLayout.stepMode = WGPUVertexStepMode_Vertex;
        vertexBufferLayout.attributeCount = 1;
        vertexBufferLayout.attributes = &positionAttribute;

        WGPUBlendState blendState = {};
        blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blendState.color.operation = WGPUBlendOperation_Add;
        blendState.alpha.srcFactor = WGPUBlendFactor_One;
        blendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blendState.alpha.operation = WGPUBlendOperation_Add;

        WGPUColorTargetState colorTargetState = {};
        colorTargetState.format = _yettyGpuContext.surfaceFormat;
        colorTargetState.blend = &blendState;
        colorTargetState.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fragmentState = {};
        fragmentState.module = _shaderModule;
        fragmentState.entryPoint = WGPU_STR("fs_main");
        fragmentState.targetCount = 1;
        fragmentState.targets = &colorTargetState;

        WGPURenderPipelineDescriptor pipelineDescriptor = {};
        pipelineDescriptor.label = WGPU_STR("terminal pipeline");
        pipelineDescriptor.layout = _pipelineLayout;
        pipelineDescriptor.vertex.module = _shaderModule;
        pipelineDescriptor.vertex.entryPoint = WGPU_STR("vs_main");
        pipelineDescriptor.vertex.bufferCount = 1;
        pipelineDescriptor.vertex.buffers = &vertexBufferLayout;
        pipelineDescriptor.fragment = &fragmentState;
        pipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        pipelineDescriptor.primitive.frontFace = WGPUFrontFace_CCW;
        pipelineDescriptor.primitive.cullMode = WGPUCullMode_None;
        pipelineDescriptor.multisample.count = 1;
        pipelineDescriptor.multisample.mask = ~0u;

        _pipeline = wgpuDeviceCreateRenderPipeline(_yettyGpuContext.device, &pipelineDescriptor);
        if (!_pipeline) {
            return Err<void>("Failed to create pipeline");
        }

        return Ok();
    }

    //=========================================================================
    // cleanup
    //=========================================================================

    void cleanup() {
        if (_pipeline) {
            wgpuRenderPipelineRelease(_pipeline);
            _pipeline = nullptr;
        }
        if (_pipelineLayout) {
            wgpuPipelineLayoutRelease(_pipelineLayout);
            _pipelineLayout = nullptr;
        }
        if (_shaderModule) {
            wgpuShaderModuleRelease(_shaderModule);
            _shaderModule = nullptr;
        }
        if (_quadVertexBuffer) {
            _gpuAllocator->releaseBuffer(_quadVertexBuffer);
            _quadVertexBuffer = nullptr;
        }
        if (_bindGroup) {
            wgpuBindGroupRelease(_bindGroup);
            _bindGroup = nullptr;
        }
        if (_bindGroupLayout) {
            wgpuBindGroupLayoutRelease(_bindGroupLayout);
            _bindGroupLayout = nullptr;
        }
        for (ResourceEntry& resourceEntry : _entries) {
            if (resourceEntry.uniformBuffer) _gpuAllocator->releaseBuffer(resourceEntry.uniformBuffer);
            if (resourceEntry.storageBuffer) _gpuAllocator->releaseBuffer(resourceEntry.storageBuffer);
            if (resourceEntry.sampler) wgpuSamplerRelease(resourceEntry.sampler);
            if (resourceEntry.textureView) wgpuTextureViewRelease(resourceEntry.textureView);
            if (resourceEntry.texture) _gpuAllocator->releaseTexture(resourceEntry.texture);
        }
        _entries.clear();
        _resourceSets.clear();
    }

    //=========================================================================
    // Data
    //=========================================================================

    YettyGpuContext _yettyGpuContext;
    GpuAllocator* _gpuAllocator;
    ShaderManager* _shaderManager;

    std::vector<GpuResourceSet> _resourceSets;
    std::vector<ResourceEntry> _entries;

    WGPUBindGroupLayout _bindGroupLayout = nullptr;
    WGPUBindGroup _bindGroup = nullptr;
    WGPUShaderModule _shaderModule = nullptr;
    WGPUPipelineLayout _pipelineLayout = nullptr;
    WGPURenderPipeline _pipeline = nullptr;
    WGPUBuffer _quadVertexBuffer = nullptr;
    bool _finalized = false;
};

//=============================================================================
// Factory
//=============================================================================

Result<GpuResourceBinder*> GpuResourceBinder::createImpl(
    const YettyGpuContext& yettyGpuContext,
    GpuAllocator* gpuAllocator,
    ShaderManager* shaderManager) {
    auto* gpuResourceBinder = new GpuResourceBinderImpl(yettyGpuContext, gpuAllocator, shaderManager);
    if (auto result = gpuResourceBinder->init(); !result) {
        delete gpuResourceBinder;
        return Err<GpuResourceBinder*>("Failed to init GpuResourceBinder", result);
    }
    return Ok(gpuResourceBinder);
}

} // namespace yetty
