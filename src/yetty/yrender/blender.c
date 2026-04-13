#include <yetty/yrender/blender.h>
#include <yetty/yrender/render-target.h>
#include <yetty/yrender/rendered-layer.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define INCBIN_STYLE 1
#include <incbin.h>

/* Embedded blend shader */
INCBIN(blend_shader, BLEND_SHADER_PATH);

#define MAX_BLEND_LAYERS 4

struct blender_impl {
    struct yetty_render_blender base;
    WGPUDevice device;
    WGPUQueue queue;

    /* Owned render target */
    struct yetty_render_target *target;

    /* Pipeline resources */
    WGPUShaderModule shader;
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;
    WGPUSampler sampler;
    WGPUBuffer uniform_buffer;

    /* Placeholder texture for unused layer slots */
    WGPUTexture placeholder_texture;
    WGPUTextureView placeholder_view;
};

static void blender_destroy(struct yetty_render_blender *self)
{
    struct blender_impl *impl = (struct blender_impl *)self;

    if (impl->target) {
        impl->target->ops->destroy(impl->target);
        impl->target = NULL;
    }

    if (impl->pipeline) {
        wgpuRenderPipelineRelease(impl->pipeline);
        impl->pipeline = NULL;
    }
    if (impl->bind_group_layout) {
        wgpuBindGroupLayoutRelease(impl->bind_group_layout);
        impl->bind_group_layout = NULL;
    }
    if (impl->shader) {
        wgpuShaderModuleRelease(impl->shader);
        impl->shader = NULL;
    }
    if (impl->sampler) {
        wgpuSamplerRelease(impl->sampler);
        impl->sampler = NULL;
    }
    if (impl->uniform_buffer) {
        wgpuBufferDestroy(impl->uniform_buffer);
        wgpuBufferRelease(impl->uniform_buffer);
        impl->uniform_buffer = NULL;
    }
    if (impl->placeholder_view) {
        wgpuTextureViewRelease(impl->placeholder_view);
        impl->placeholder_view = NULL;
    }
    if (impl->placeholder_texture) {
        wgpuTextureDestroy(impl->placeholder_texture);
        wgpuTextureRelease(impl->placeholder_texture);
        impl->placeholder_texture = NULL;
    }

    free(impl);
}

static struct yetty_core_void_result create_pipeline(struct blender_impl *impl)
{
    /* Create shader module */
    WGPUShaderSourceWGSL wgsl_src = {0};
    wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_src.code = (WGPUStringView){.data = (const char *)gblend_shader_data, .length = gblend_shader_size};

    WGPUShaderModuleDescriptor shader_desc = {0};
    shader_desc.nextInChain = (WGPUChainedStruct *)&wgsl_src;

    impl->shader = wgpuDeviceCreateShaderModule(impl->device, &shader_desc);
    if (!impl->shader) {
        return YETTY_ERR(yetty_core_void, "failed to create blend shader");
    }

    /* Create bind group layout */
    WGPUBindGroupLayoutEntry entries[6] = {0};

    /* Layer textures (0-3) */
    for (int i = 0; i < 4; i++) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].texture.sampleType = WGPUTextureSampleType_Float;
        entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }

    /* Sampler (4) */
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Fragment;
    entries[4].sampler.type = WGPUSamplerBindingType_Filtering;

    /* Uniforms (5) */
    entries[5].binding = 5;
    entries[5].visibility = WGPUShaderStage_Fragment;
    entries[5].buffer.type = WGPUBufferBindingType_Uniform;
    entries[5].buffer.minBindingSize = 16;

    WGPUBindGroupLayoutDescriptor bgl_desc = {0};
    bgl_desc.entryCount = 6;
    bgl_desc.entries = entries;

    impl->bind_group_layout = wgpuDeviceCreateBindGroupLayout(impl->device, &bgl_desc);
    if (!impl->bind_group_layout) {
        return YETTY_ERR(yetty_core_void, "failed to create bind group layout");
    }

    /* Create pipeline layout */
    WGPUPipelineLayoutDescriptor pl_desc = {0};
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &impl->bind_group_layout;

    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(impl->device, &pl_desc);
    if (!pipeline_layout) {
        return YETTY_ERR(yetty_core_void, "failed to create pipeline layout");
    }

    /* Create render pipeline */
    WGPURenderPipelineDescriptor rp_desc = {0};
    rp_desc.layout = pipeline_layout;

    rp_desc.vertex.module = impl->shader;
    rp_desc.vertex.entryPoint = (WGPUStringView){.data = "vs_main", .length = 7};

    WGPUColorTargetState color_target = {0};
    color_target.format = impl->target->ops->get_format(impl->target);
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = {0};
    fragment.module = impl->shader;
    fragment.entryPoint = (WGPUStringView){.data = "fs_main", .length = 7};
    fragment.targetCount = 1;
    fragment.targets = &color_target;
    rp_desc.fragment = &fragment;

    rp_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rp_desc.primitive.frontFace = WGPUFrontFace_CCW;
    rp_desc.primitive.cullMode = WGPUCullMode_None;

    rp_desc.multisample.count = 1;
    rp_desc.multisample.mask = ~0u;

    impl->pipeline = wgpuDeviceCreateRenderPipeline(impl->device, &rp_desc);
    wgpuPipelineLayoutRelease(pipeline_layout);

    if (!impl->pipeline) {
        return YETTY_ERR(yetty_core_void, "failed to create blend pipeline");
    }

    /* Create sampler */
    WGPUSamplerDescriptor sampler_desc = {0};
    sampler_desc.minFilter = WGPUFilterMode_Linear;
    sampler_desc.magFilter = WGPUFilterMode_Linear;
    sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_desc.maxAnisotropy = 1;

    impl->sampler = wgpuDeviceCreateSampler(impl->device, &sampler_desc);
    if (!impl->sampler) {
        return YETTY_ERR(yetty_core_void, "failed to create sampler");
    }

    /* Create uniform buffer */
    WGPUBufferDescriptor buf_desc = {0};
    buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    buf_desc.size = 16;

    impl->uniform_buffer = wgpuDeviceCreateBuffer(impl->device, &buf_desc);
    if (!impl->uniform_buffer) {
        return YETTY_ERR(yetty_core_void, "failed to create uniform buffer");
    }

    /* Create placeholder texture for unused layer slots */
    WGPUTextureDescriptor tex_desc = {0};
    tex_desc.usage = WGPUTextureUsage_TextureBinding;
    tex_desc.dimension = WGPUTextureDimension_2D;
    tex_desc.size.width = 1;
    tex_desc.size.height = 1;
    tex_desc.size.depthOrArrayLayers = 1;
    tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;

    impl->placeholder_texture = wgpuDeviceCreateTexture(impl->device, &tex_desc);
    if (!impl->placeholder_texture) {
        return YETTY_ERR(yetty_core_void, "failed to create placeholder texture");
    }

    impl->placeholder_view = wgpuTextureCreateView(impl->placeholder_texture, NULL);
    if (!impl->placeholder_view) {
        return YETTY_ERR(yetty_core_void, "failed to create placeholder view");
    }

    ydebug("blender: pipeline created");
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result blender_blend(
    struct yetty_render_blender *self,
    struct yetty_render_rendered_layer **layers,
    size_t layer_count)
{
    struct blender_impl *impl = (struct blender_impl *)self;

    if (!impl->target) {
        return YETTY_ERR(yetty_core_void, "no render target");
    }

    if (layer_count == 0) {
        return YETTY_OK_VOID();
    }

    if (layer_count > MAX_BLEND_LAYERS) {
        yerror("blender: too many layers (%zu > %d)", layer_count, MAX_BLEND_LAYERS);
        layer_count = MAX_BLEND_LAYERS;
    }

    /* Create pipeline on first use */
    if (!impl->pipeline) {
        struct yetty_core_void_result res = create_pipeline(impl);
        if (!YETTY_IS_OK(res)) {
            return res;
        }
    }

    /* Acquire target view */
    WGPUTextureView target_view = impl->target->ops->acquire(impl->target);
    if (!target_view) {
        return YETTY_ERR(yetty_core_void, "failed to acquire target");
    }

    /* Update uniform buffer */
    uint32_t uniforms[4] = {(uint32_t)layer_count, 0, 0, 0};
    wgpuQueueWriteBuffer(impl->queue, impl->uniform_buffer, 0, uniforms, sizeof(uniforms));

    /* Create bind group with layer textures */
    WGPUTextureView layer_views[MAX_BLEND_LAYERS];
    for (size_t i = 0; i < MAX_BLEND_LAYERS; i++) {
        if (i < layer_count && layers[i]) {
            layer_views[i] = layers[i]->ops->get_view(layers[i]);
        } else {
            layer_views[i] = impl->placeholder_view;
        }
    }

    WGPUBindGroupEntry bg_entries[6] = {0};
    for (int i = 0; i < 4; i++) {
        bg_entries[i].binding = i;
        bg_entries[i].textureView = layer_views[i];
    }
    bg_entries[4].binding = 4;
    bg_entries[4].sampler = impl->sampler;
    bg_entries[5].binding = 5;
    bg_entries[5].buffer = impl->uniform_buffer;
    bg_entries[5].size = 16;

    WGPUBindGroupDescriptor bg_desc = {0};
    bg_desc.layout = impl->bind_group_layout;
    bg_desc.entryCount = 6;
    bg_desc.entries = bg_entries;

    WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(impl->device, &bg_desc);
    if (!bind_group) {
        return YETTY_ERR(yetty_core_void, "failed to create bind group");
    }

    /* Create command encoder */
    WGPUCommandEncoderDescriptor enc_desc = {0};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl->device, &enc_desc);
    if (!encoder) {
        wgpuBindGroupRelease(bind_group);
        return YETTY_ERR(yetty_core_void, "failed to create encoder");
    }

    /* Begin render pass */
    WGPURenderPassColorAttachment color_attachment = {0};
    color_attachment.view = target_view;
    color_attachment.loadOp = WGPULoadOp_Clear;
    color_attachment.storeOp = WGPUStoreOp_Store;
    color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 1.0};
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    WGPURenderPassDescriptor pass_desc = {0};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    if (!pass) {
        wgpuCommandEncoderRelease(encoder);
        wgpuBindGroupRelease(bind_group);
        return YETTY_ERR(yetty_core_void, "failed to begin render pass");
    }

    /* Draw full-screen quad */
    wgpuRenderPassEncoderSetPipeline(pass, impl->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    /* Submit */
    WGPUCommandBufferDescriptor cmd_desc = {0};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    wgpuQueueSubmit(impl->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(bind_group);

    /* Present */
    impl->target->ops->present(impl->target);

    ydebug("blender_blend: blended %zu layers", layer_count);
    return YETTY_OK_VOID();
}

static void blender_set_target(
    struct yetty_render_blender *self,
    struct yetty_render_target *target)
{
    struct blender_impl *impl = (struct blender_impl *)self;

    /* Destroy old target */
    if (impl->target) {
        impl->target->ops->destroy(impl->target);
    }

    impl->target = target;

    /* Invalidate pipeline (format may have changed) */
    if (impl->pipeline) {
        wgpuRenderPipelineRelease(impl->pipeline);
        impl->pipeline = NULL;
    }

    ydebug("blender_set_target: target replaced");
}

static struct yetty_render_target *blender_get_target(
    const struct yetty_render_blender *self)
{
    const struct blender_impl *impl = (const struct blender_impl *)self;
    return impl->target;
}

static const struct yetty_render_blender_ops blender_ops = {
    .destroy = blender_destroy,
    .blend = blender_blend,
    .set_target = blender_set_target,
    .get_target = blender_get_target,
};

struct yetty_render_blender_result yetty_render_blender_create(
    WGPUDevice device,
    WGPUQueue queue,
    struct yetty_render_target *target)
{
    struct blender_impl *impl = calloc(1, sizeof(struct blender_impl));
    if (!impl)
        return YETTY_ERR(yetty_render_blender, "failed to allocate blender");

    impl->base.ops = &blender_ops;
    impl->device = device;
    impl->queue = queue;
    impl->target = target;  /* takes ownership */

    ydebug("blender_create: created with target");
    return YETTY_OK(yetty_render_blender, &impl->base);
}
