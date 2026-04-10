#include <yetty/render/gpu-resource-binder.h>
#include <yetty/render/gpu-allocator.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_RESOURCE_SETS 32
#define MAX_BINDING_CODE 8192

struct resource_entry {
    WGPUBuffer uniform_buffer;
    size_t uniform_size;
    WGPUTexture texture;
    WGPUTextureView texture_view;
    uint32_t texture_width;
    uint32_t texture_height;
    WGPUTextureFormat texture_format;
    WGPUSampler sampler;
    WGPUBuffer storage_buffer;
    size_t storage_buffer_size;
    int storage_buffer_readonly;
};

struct gpu_resource_binder_impl {
    struct yetty_render_gpu_resource_binder base;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat surface_format;
    struct yetty_render_gpu_allocator *allocator;

    struct yetty_render_gpu_resource_set resource_sets[MAX_RESOURCE_SETS];
    struct resource_entry entries[MAX_RESOURCE_SETS];
    size_t resource_set_count;

    char shader_code[32768];
    size_t shader_code_size;

    WGPUBindGroupLayout bind_group_layout;
    WGPUBindGroup bind_group;
    WGPUShaderModule shader_module;
    WGPUPipelineLayout pipeline_layout;
    WGPURenderPipeline pipeline;
    WGPUBuffer quad_vertex_buffer;
    int finalized;
};

/* Forward declarations */
static void binder_destroy(struct yetty_render_gpu_resource_binder *self);
static struct yetty_core_void_result binder_submit(struct yetty_render_gpu_resource_binder *self,
                                                    const struct yetty_render_gpu_resource_set *rs);
static struct yetty_core_void_result binder_finalize(struct yetty_render_gpu_resource_binder *self);
static struct yetty_core_void_result binder_bind(struct yetty_render_gpu_resource_binder *self,
                                                  WGPURenderPassEncoder pass, uint32_t group_index);
static WGPURenderPipeline binder_get_pipeline(const struct yetty_render_gpu_resource_binder *self);
static WGPUBuffer binder_get_quad_vertex_buffer(const struct yetty_render_gpu_resource_binder *self);

static const struct yetty_render_gpu_resource_binder_ops binder_ops = {
    .destroy = binder_destroy,
    .submit = binder_submit,
    .finalize = binder_finalize,
    .bind = binder_bind,
    .get_pipeline = binder_get_pipeline,
    .get_quad_vertex_buffer = binder_get_quad_vertex_buffer,
};

static struct yetty_core_void_result create_resources(struct gpu_resource_binder_impl *impl, size_t index);
static struct yetty_core_void_result upload_data(struct gpu_resource_binder_impl *impl, size_t index);
static struct yetty_core_void_result create_bind_group(struct gpu_resource_binder_impl *impl);
static struct yetty_core_void_result compile_and_create_pipeline(struct gpu_resource_binder_impl *impl);
static void generate_wgsl_bindings(struct gpu_resource_binder_impl *impl, char *out, size_t out_size);

static void binder_destroy(struct yetty_render_gpu_resource_binder *self)
{
    struct gpu_resource_binder_impl *impl = (struct gpu_resource_binder_impl *)self;

    if (impl->pipeline)
        wgpuRenderPipelineRelease(impl->pipeline);
    if (impl->pipeline_layout)
        wgpuPipelineLayoutRelease(impl->pipeline_layout);
    if (impl->shader_module)
        wgpuShaderModuleRelease(impl->shader_module);
    if (impl->quad_vertex_buffer && impl->allocator)
        impl->allocator->ops->release_buffer(impl->allocator, impl->quad_vertex_buffer);
    if (impl->bind_group)
        wgpuBindGroupRelease(impl->bind_group);
    if (impl->bind_group_layout)
        wgpuBindGroupLayoutRelease(impl->bind_group_layout);

    for (size_t i = 0; i < impl->resource_set_count; i++) {
        struct resource_entry *e = &impl->entries[i];
        if (e->uniform_buffer && impl->allocator)
            impl->allocator->ops->release_buffer(impl->allocator, e->uniform_buffer);
        if (e->storage_buffer && impl->allocator)
            impl->allocator->ops->release_buffer(impl->allocator, e->storage_buffer);
        if (e->sampler)
            wgpuSamplerRelease(e->sampler);
        if (e->texture_view)
            wgpuTextureViewRelease(e->texture_view);
        if (e->texture && impl->allocator)
            impl->allocator->ops->release_texture(impl->allocator, e->texture);
    }

    free(impl);
}

static struct yetty_core_void_result binder_submit(struct yetty_render_gpu_resource_binder *self,
                                                    const struct yetty_render_gpu_resource_set *rs)
{
    struct gpu_resource_binder_impl *impl = (struct gpu_resource_binder_impl *)self;

    /* Find existing by namespace */
    for (size_t i = 0; i < impl->resource_set_count; i++) {
        if (strcmp(impl->resource_sets[i].namespace, rs->namespace) == 0) {
            impl->resource_sets[i] = *rs;
            return upload_data(impl, i);
        }
    }

    /* New resource set */
    if (impl->resource_set_count >= MAX_RESOURCE_SETS)
        return YETTY_ERR(yetty_core_void, "max resource sets reached");

    size_t index = impl->resource_set_count++;
    impl->resource_sets[index] = *rs;
    memset(&impl->entries[index], 0, sizeof(struct resource_entry));

    /* Store shader code from first resource set that has it */
    if (rs->shader_code && rs->shader_code_size > 0 && impl->shader_code_size == 0) {
        size_t copy_size = rs->shader_code_size;
        if (copy_size >= sizeof(impl->shader_code))
            copy_size = sizeof(impl->shader_code) - 1;
        memcpy(impl->shader_code, rs->shader_code, copy_size);
        impl->shader_code[copy_size] = '\0';
        impl->shader_code_size = copy_size;
        ydebug("GpuResourceBinder: stored shader code %zu bytes from '%s'", copy_size, rs->namespace);
    }

    struct yetty_core_void_result res = create_resources(impl, index);
    if (!YETTY_IS_OK(res))
        return res;

    return upload_data(impl, index);
}

static struct yetty_core_void_result binder_finalize(struct yetty_render_gpu_resource_binder *self)
{
    struct gpu_resource_binder_impl *impl = (struct gpu_resource_binder_impl *)self;

    if (impl->finalized)
        return YETTY_OK_VOID();

    struct yetty_core_void_result res = create_bind_group(impl);
    if (!YETTY_IS_OK(res))
        return res;

    res = compile_and_create_pipeline(impl);
    if (!YETTY_IS_OK(res))
        return res;

    impl->finalized = 1;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result binder_bind(struct yetty_render_gpu_resource_binder *self,
                                                  WGPURenderPassEncoder pass, uint32_t group_index)
{
    struct gpu_resource_binder_impl *impl = (struct gpu_resource_binder_impl *)self;

    if (!impl->finalized)
        return YETTY_ERR(yetty_core_void, "not finalized");
    if (!impl->bind_group)
        return YETTY_ERR(yetty_core_void, "bind group is null");

    wgpuRenderPassEncoderSetBindGroup(pass, group_index, impl->bind_group, 0, NULL);
    return YETTY_OK_VOID();
}

static WGPURenderPipeline binder_get_pipeline(const struct yetty_render_gpu_resource_binder *self)
{
    const struct gpu_resource_binder_impl *impl = (const struct gpu_resource_binder_impl *)self;
    return impl->pipeline;
}

static WGPUBuffer binder_get_quad_vertex_buffer(const struct yetty_render_gpu_resource_binder *self)
{
    const struct gpu_resource_binder_impl *impl = (const struct gpu_resource_binder_impl *)self;
    return impl->quad_vertex_buffer;
}

static struct yetty_core_void_result create_resources(struct gpu_resource_binder_impl *impl, size_t index)
{
    const struct yetty_render_gpu_resource_set *rs = &impl->resource_sets[index];
    struct resource_entry *e = &impl->entries[index];

    if (rs->uniform_size > 0) {
        char label[128];
        snprintf(label, sizeof(label), "%s_uniform", rs->name);
        WGPUBufferDescriptor desc = {0};
        desc.label.data = label;
        desc.label.length = strlen(label);
        desc.size = rs->uniform_size;
        desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        e->uniform_buffer = impl->allocator->ops->create_buffer(impl->allocator, &desc);
        if (!e->uniform_buffer)
            return YETTY_ERR(yetty_core_void, "failed to create uniform buffer");
        e->uniform_size = rs->uniform_size;
    }

    if (rs->texture_width > 0 && rs->texture_height > 0) {
        WGPUTextureDescriptor tex_desc = {0};
        tex_desc.label.data = rs->name;
        tex_desc.label.length = strlen(rs->name);
        tex_desc.size.width = rs->texture_width;
        tex_desc.size.height = rs->texture_height;
        tex_desc.size.depthOrArrayLayers = 1;
        tex_desc.mipLevelCount = 1;
        tex_desc.sampleCount = 1;
        tex_desc.dimension = WGPUTextureDimension_2D;
        tex_desc.format = rs->texture_format;
        tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

        e->texture = impl->allocator->ops->create_texture(impl->allocator, &tex_desc);
        if (!e->texture)
            return YETTY_ERR(yetty_core_void, "failed to create texture");
        e->texture_width = rs->texture_width;
        e->texture_height = rs->texture_height;
        e->texture_format = rs->texture_format;

        WGPUTextureViewDescriptor view_desc = {0};
        view_desc.format = rs->texture_format;
        view_desc.dimension = WGPUTextureViewDimension_2D;
        view_desc.mipLevelCount = 1;
        view_desc.arrayLayerCount = 1;
        e->texture_view = wgpuTextureCreateView(e->texture, &view_desc);

        WGPUSamplerDescriptor sampler_desc = {0};
        sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
        sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
        sampler_desc.addressModeW = WGPUAddressMode_ClampToEdge;
        sampler_desc.magFilter = rs->sampler_filter;
        sampler_desc.minFilter = rs->sampler_filter;
        sampler_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sampler_desc.maxAnisotropy = 1;
        e->sampler = wgpuDeviceCreateSampler(impl->device, &sampler_desc);
    }

    if (rs->buffer_size > 0) {
        char label[128];
        snprintf(label, sizeof(label), "%s_storage", rs->name);
        WGPUBufferDescriptor desc = {0};
        desc.label.data = label;
        desc.label.length = strlen(label);
        desc.size = rs->buffer_size;
        desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        e->storage_buffer = impl->allocator->ops->create_buffer(impl->allocator, &desc);
        if (!e->storage_buffer)
            return YETTY_ERR(yetty_core_void, "failed to create storage buffer");
        e->storage_buffer_size = rs->buffer_size;
        e->storage_buffer_readonly = rs->buffer_readonly;
    }

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result upload_data(struct gpu_resource_binder_impl *impl, size_t index)
{
    const struct yetty_render_gpu_resource_set *rs = &impl->resource_sets[index];
    const struct resource_entry *e = &impl->entries[index];

    if (e->uniform_buffer && rs->uniform_data && rs->uniform_data_size > 0) {
        wgpuQueueWriteBuffer(impl->queue, e->uniform_buffer, 0,
                             rs->uniform_data, rs->uniform_data_size);
    }

    if (e->texture && rs->texture_data && rs->texture_data_size > 0) {
        WGPUTexelCopyTextureInfo dest = {0};
        dest.texture = e->texture;
        dest.mipLevel = 0;
        dest.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout src_layout = {0};
        src_layout.bytesPerRow = e->texture_width * 4; /* assume RGBA */
        src_layout.rowsPerImage = e->texture_height;

        WGPUExtent3D extent = {e->texture_width, e->texture_height, 1};
        wgpuQueueWriteTexture(impl->queue, &dest, rs->texture_data, rs->texture_data_size,
                              &src_layout, &extent);
    }

    if (e->storage_buffer && rs->buffer_data && rs->buffer_data_size > 0) {
        wgpuQueueWriteBuffer(impl->queue, e->storage_buffer, 0,
                             rs->buffer_data, rs->buffer_data_size);
    }

    return YETTY_OK_VOID();
}

static void generate_wgsl_bindings(struct gpu_resource_binder_impl *impl, char *out, size_t out_size)
{
    char *p = out;
    size_t remaining = out_size;
    uint32_t binding = 0;

    for (size_t i = 0; i < impl->resource_set_count; i++) {
        const struct yetty_render_gpu_resource_set *rs = &impl->resource_sets[i];

        if (rs->uniform_size > 0 && rs->uniform_wgsl_type[0]) {
            const char *name = rs->uniform_name[0] ? rs->uniform_name : rs->name;
            int n = snprintf(p, remaining, "@group(0) @binding(%u) var<uniform> %s: %s;\n",
                             binding++, name, rs->uniform_wgsl_type);
            if (n > 0 && (size_t)n < remaining) { p += n; remaining -= n; }
        }

        if (rs->texture_width > 0 && rs->texture_height > 0) {
            const char *tex_type = rs->texture_wgsl_type[0] ? rs->texture_wgsl_type : "texture_2d<f32>";
            const char *tex_name = rs->texture_name[0] ? rs->texture_name : "texture";
            const char *smp_name = rs->sampler_name[0] ? rs->sampler_name : "textureSampler";
            int n = snprintf(p, remaining, "@group(0) @binding(%u) var %s: %s;\n",
                             binding++, tex_name, tex_type);
            if (n > 0 && (size_t)n < remaining) { p += n; remaining -= n; }
            n = snprintf(p, remaining, "@group(0) @binding(%u) var %s: sampler;\n",
                         binding++, smp_name);
            if (n > 0 && (size_t)n < remaining) { p += n; remaining -= n; }
        }

        if (rs->buffer_size > 0) {
            const char *buf_type = rs->buffer_wgsl_type[0] ? rs->buffer_wgsl_type : "array<u32>";
            const char *buf_name = rs->buffer_name[0] ? rs->buffer_name : "buffer";
            const char *access = rs->buffer_readonly ? "read" : "read_write";
            int n = snprintf(p, remaining, "@group(0) @binding(%u) var<storage, %s> %s: %s;\n",
                             binding++, access, buf_name, buf_type);
            if (n > 0 && (size_t)n < remaining) { p += n; remaining -= n; }
        }
    }
}

static struct yetty_core_void_result create_bind_group(struct gpu_resource_binder_impl *impl)
{
    if (impl->resource_set_count == 0)
        return YETTY_ERR(yetty_core_void, "no resource sets");

    WGPUBindGroupLayoutEntry layout_entries[64];
    WGPUBindGroupEntry group_entries[64];
    size_t entry_count = 0;
    uint32_t binding = 0;

    for (size_t i = 0; i < impl->resource_set_count; i++) {
        const struct resource_entry *e = &impl->entries[i];

        if (e->uniform_buffer) {
            WGPUBindGroupLayoutEntry *le = &layout_entries[entry_count];
            memset(le, 0, sizeof(*le));
            le->binding = binding;
            le->visibility = WGPUShaderStage_Fragment | WGPUShaderStage_Vertex;
            le->buffer.type = WGPUBufferBindingType_Uniform;

            WGPUBindGroupEntry *ge = &group_entries[entry_count];
            memset(ge, 0, sizeof(*ge));
            ge->binding = binding;
            ge->buffer = e->uniform_buffer;
            ge->size = e->uniform_size;

            entry_count++;
            binding++;
        }

        if (e->texture_view) {
            WGPUBindGroupLayoutEntry *le = &layout_entries[entry_count];
            memset(le, 0, sizeof(*le));
            le->binding = binding;
            le->visibility = WGPUShaderStage_Fragment;
            le->texture.sampleType = WGPUTextureSampleType_Float;
            le->texture.viewDimension = WGPUTextureViewDimension_2D;

            WGPUBindGroupEntry *ge = &group_entries[entry_count];
            memset(ge, 0, sizeof(*ge));
            ge->binding = binding;
            ge->textureView = e->texture_view;

            entry_count++;
            binding++;
        }

        if (e->sampler) {
            WGPUBindGroupLayoutEntry *le = &layout_entries[entry_count];
            memset(le, 0, sizeof(*le));
            le->binding = binding;
            le->visibility = WGPUShaderStage_Fragment;
            le->sampler.type = WGPUSamplerBindingType_Filtering;

            WGPUBindGroupEntry *ge = &group_entries[entry_count];
            memset(ge, 0, sizeof(*ge));
            ge->binding = binding;
            ge->sampler = e->sampler;

            entry_count++;
            binding++;
        }

        if (e->storage_buffer) {
            WGPUBindGroupLayoutEntry *le = &layout_entries[entry_count];
            memset(le, 0, sizeof(*le));
            le->binding = binding;
            le->visibility = WGPUShaderStage_Fragment;
            le->buffer.type = e->storage_buffer_readonly
                ? WGPUBufferBindingType_ReadOnlyStorage
                : WGPUBufferBindingType_Storage;

            WGPUBindGroupEntry *ge = &group_entries[entry_count];
            memset(ge, 0, sizeof(*ge));
            ge->binding = binding;
            ge->buffer = e->storage_buffer;
            ge->size = e->storage_buffer_size;

            entry_count++;
            binding++;
        }
    }

    if (entry_count == 0)
        return YETTY_ERR(yetty_core_void, "no bindings");

    WGPUBindGroupLayoutDescriptor layout_desc = {0};
    layout_desc.entryCount = entry_count;
    layout_desc.entries = layout_entries;
    impl->bind_group_layout = wgpuDeviceCreateBindGroupLayout(impl->device, &layout_desc);
    if (!impl->bind_group_layout)
        return YETTY_ERR(yetty_core_void, "failed to create bind group layout");

    WGPUBindGroupDescriptor group_desc = {0};
    group_desc.layout = impl->bind_group_layout;
    group_desc.entryCount = entry_count;
    group_desc.entries = group_entries;
    impl->bind_group = wgpuDeviceCreateBindGroup(impl->device, &group_desc);
    if (!impl->bind_group)
        return YETTY_ERR(yetty_core_void, "failed to create bind group");

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result compile_and_create_pipeline(struct gpu_resource_binder_impl *impl)
{
    if (impl->shader_code_size == 0)
        return YETTY_ERR(yetty_core_void, "no shader code");

    /* Generate bindings and prepend to shader code */
    char bindings[MAX_BINDING_CODE];
    generate_wgsl_bindings(impl, bindings, sizeof(bindings));

    size_t bindings_len = strlen(bindings);
    size_t total_len = bindings_len + impl->shader_code_size + 2;
    char *merged = malloc(total_len);
    if (!merged)
        return YETTY_ERR(yetty_core_void, "failed to allocate merged shader");

    memcpy(merged, bindings, bindings_len);
    merged[bindings_len] = '\n';
    memcpy(merged + bindings_len + 1, impl->shader_code, impl->shader_code_size);
    merged[total_len - 1] = '\0';

    ydebug("GpuResourceBinder: merged shader %zu bytes", total_len);

    WGPUShaderSourceWGSL wgsl_src = {0};
    wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_src.code.data = merged;
    wgsl_src.code.length = total_len - 1;

    WGPUShaderModuleDescriptor shader_desc = {0};
    shader_desc.label.data = "terminal shader";
    shader_desc.label.length = 15;
    shader_desc.nextInChain = &wgsl_src.chain;

    impl->shader_module = wgpuDeviceCreateShaderModule(impl->device, &shader_desc);
    free(merged);

    if (!impl->shader_module)
        return YETTY_ERR(yetty_core_void, "failed to compile shader");

    /* Quad vertex buffer */
    float quad_vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
    };
    WGPUBufferDescriptor quad_desc = {0};
    quad_desc.label.data = "quad vertices";
    quad_desc.label.length = 13;
    quad_desc.size = sizeof(quad_vertices);
    quad_desc.usage = WGPUBufferUsage_Vertex;
    quad_desc.mappedAtCreation = 1;
    impl->quad_vertex_buffer = impl->allocator->ops->create_buffer(impl->allocator, &quad_desc);
    if (!impl->quad_vertex_buffer)
        return YETTY_ERR(yetty_core_void, "failed to create quad buffer");

    void *mapped = wgpuBufferGetMappedRange(impl->quad_vertex_buffer, 0, sizeof(quad_vertices));
    memcpy(mapped, quad_vertices, sizeof(quad_vertices));
    wgpuBufferUnmap(impl->quad_vertex_buffer);

    /* Pipeline layout */
    WGPUPipelineLayoutDescriptor pl_desc = {0};
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &impl->bind_group_layout;
    impl->pipeline_layout = wgpuDeviceCreatePipelineLayout(impl->device, &pl_desc);
    if (!impl->pipeline_layout)
        return YETTY_ERR(yetty_core_void, "failed to create pipeline layout");

    /* Pipeline */
    WGPUVertexAttribute pos_attr = {0};
    pos_attr.format = WGPUVertexFormat_Float32x2;
    pos_attr.offset = 0;
    pos_attr.shaderLocation = 0;

    WGPUVertexBufferLayout vb_layout = {0};
    vb_layout.arrayStride = 2 * sizeof(float);
    vb_layout.stepMode = WGPUVertexStepMode_Vertex;
    vb_layout.attributeCount = 1;
    vb_layout.attributes = &pos_attr;

    WGPUBlendState blend = {0};
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;

    WGPUColorTargetState color_target = {0};
    color_target.format = impl->surface_format;
    color_target.blend = &blend;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState frag = {0};
    frag.module = impl->shader_module;
    frag.entryPoint.data = "fs_main";
    frag.entryPoint.length = 7;
    frag.targetCount = 1;
    frag.targets = &color_target;

    WGPURenderPipelineDescriptor pipe_desc = {0};
    pipe_desc.label.data = "terminal pipeline";
    pipe_desc.label.length = 17;
    pipe_desc.layout = impl->pipeline_layout;
    pipe_desc.vertex.module = impl->shader_module;
    pipe_desc.vertex.entryPoint.data = "vs_main";
    pipe_desc.vertex.entryPoint.length = 7;
    pipe_desc.vertex.bufferCount = 1;
    pipe_desc.vertex.buffers = &vb_layout;
    pipe_desc.fragment = &frag;
    pipe_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipe_desc.primitive.frontFace = WGPUFrontFace_CCW;
    pipe_desc.primitive.cullMode = WGPUCullMode_None;
    pipe_desc.multisample.count = 1;
    pipe_desc.multisample.mask = ~0u;

    impl->pipeline = wgpuDeviceCreateRenderPipeline(impl->device, &pipe_desc);
    if (!impl->pipeline)
        return YETTY_ERR(yetty_core_void, "failed to create pipeline");

    ydebug("GpuResourceBinder: pipeline created");
    return YETTY_OK_VOID();
}

struct yetty_render_gpu_resource_binder_result yetty_render_gpu_resource_binder_create(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUTextureFormat surface_format,
    struct yetty_render_gpu_allocator *allocator)
{
    if (!device)
        return YETTY_ERR(yetty_render_gpu_resource_binder, "device is null");
    if (!queue)
        return YETTY_ERR(yetty_render_gpu_resource_binder, "queue is null");
    if (!allocator)
        return YETTY_ERR(yetty_render_gpu_resource_binder, "allocator is null");

    struct gpu_resource_binder_impl *impl = calloc(1, sizeof(struct gpu_resource_binder_impl));
    if (!impl)
        return YETTY_ERR(yetty_render_gpu_resource_binder, "failed to allocate");

    impl->base.ops = &binder_ops;
    impl->device = device;
    impl->queue = queue;
    impl->surface_format = surface_format;
    impl->allocator = allocator;

    return YETTY_OK(yetty_render_gpu_resource_binder, &impl->base);
}
