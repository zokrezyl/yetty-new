/*
 * render-target-surface.c - Surface render target implementation
 *
 * This is the surface specialization of render_target. It accepts gpu_resource_sets,
 * renders each to intermediate textures via layer_renderers, and composites them
 * to the surface using a blend pass.
 *
 * For ymux, a different render_target serializes resource_sets instead of rendering.
 */

#include <yetty/yrender/render-target.h>
#include <yetty/yrender/layer-renderer.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define INCBIN_STYLE 1
#include <incbin.h>

INCBIN(blend_shader, BLEND_SHADER_PATH);

#define MAX_LAYERS 4

struct surface_render_target {
    struct yetty_render_target base;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUSurface surface;
    WGPUTextureFormat format;
    struct pixel_size size;

    /* Layer renderers (created dynamically as needed) */
    struct yetty_render_layer_renderer *layer_renderers[MAX_LAYERS];

    /* Blend pipeline resources */
    WGPUShaderModule blend_shader;
    WGPURenderPipeline blend_pipeline;
    WGPUBindGroupLayout blend_layout;
    WGPUSampler sampler;
    WGPUBuffer uniform_buffer;
    WGPUTexture placeholder_texture;
    WGPUTextureView placeholder_view;
};

static void surface_render_target_destroy(struct yetty_render_target *self)
{
    struct surface_render_target *st = (struct surface_render_target *)self;

    for (size_t i = 0; i < MAX_LAYERS; i++) {
        if (st->layer_renderers[i]) {
            st->layer_renderers[i]->ops->destroy(st->layer_renderers[i]);
            st->layer_renderers[i] = NULL;
        }
    }

    if (st->blend_pipeline) {
        wgpuRenderPipelineRelease(st->blend_pipeline);
    }
    if (st->blend_layout) {
        wgpuBindGroupLayoutRelease(st->blend_layout);
    }
    if (st->blend_shader) {
        wgpuShaderModuleRelease(st->blend_shader);
    }
    if (st->sampler) {
        wgpuSamplerRelease(st->sampler);
    }
    if (st->uniform_buffer) {
        wgpuBufferDestroy(st->uniform_buffer);
        wgpuBufferRelease(st->uniform_buffer);
    }
    if (st->placeholder_view) {
        wgpuTextureViewRelease(st->placeholder_view);
    }
    if (st->placeholder_texture) {
        wgpuTextureDestroy(st->placeholder_texture);
        wgpuTextureRelease(st->placeholder_texture);
    }

    free(st);
}

static struct yetty_core_void_result create_blend_pipeline(struct surface_render_target *st)
{
    /* Shader module */
    WGPUShaderSourceWGSL wgsl_src = {0};
    wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_src.code = (WGPUStringView){
        .data = (const char *)gblend_shader_data,
        .length = gblend_shader_size
    };

    WGPUShaderModuleDescriptor shader_desc = {0};
    shader_desc.nextInChain = (WGPUChainedStruct *)&wgsl_src;

    st->blend_shader = wgpuDeviceCreateShaderModule(st->device, &shader_desc);
    if (!st->blend_shader) {
        return YETTY_ERR(yetty_core_void, "failed to create blend shader");
    }

    /* Bind group layout: 4 textures + sampler + uniforms */
    WGPUBindGroupLayoutEntry entries[6] = {0};
    for (int i = 0; i < 4; i++) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].texture.sampleType = WGPUTextureSampleType_Float;
        entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Fragment;
    entries[4].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[5].binding = 5;
    entries[5].visibility = WGPUShaderStage_Fragment;
    entries[5].buffer.type = WGPUBufferBindingType_Uniform;
    entries[5].buffer.minBindingSize = 16;

    WGPUBindGroupLayoutDescriptor bgl_desc = {0};
    bgl_desc.entryCount = 6;
    bgl_desc.entries = entries;

    st->blend_layout = wgpuDeviceCreateBindGroupLayout(st->device, &bgl_desc);
    if (!st->blend_layout) {
        return YETTY_ERR(yetty_core_void, "failed to create blend layout");
    }

    /* Pipeline layout */
    WGPUPipelineLayoutDescriptor pl_desc = {0};
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &st->blend_layout;

    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(st->device, &pl_desc);
    if (!layout) {
        return YETTY_ERR(yetty_core_void, "failed to create pipeline layout");
    }

    /* Render pipeline */
    WGPURenderPipelineDescriptor rp_desc = {0};
    rp_desc.layout = layout;
    rp_desc.vertex.module = st->blend_shader;
    rp_desc.vertex.entryPoint = (WGPUStringView){.data = "vs_main", .length = 7};

    WGPUColorTargetState color_target = {0};
    color_target.format = st->format;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = {0};
    fragment.module = st->blend_shader;
    fragment.entryPoint = (WGPUStringView){.data = "fs_main", .length = 7};
    fragment.targetCount = 1;
    fragment.targets = &color_target;
    rp_desc.fragment = &fragment;

    rp_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rp_desc.primitive.frontFace = WGPUFrontFace_CCW;
    rp_desc.primitive.cullMode = WGPUCullMode_None;
    rp_desc.multisample.count = 1;
    rp_desc.multisample.mask = ~0u;

    st->blend_pipeline = wgpuDeviceCreateRenderPipeline(st->device, &rp_desc);
    wgpuPipelineLayoutRelease(layout);

    if (!st->blend_pipeline) {
        return YETTY_ERR(yetty_core_void, "failed to create blend pipeline");
    }

    /* Sampler */
    WGPUSamplerDescriptor sampler_desc = {0};
    sampler_desc.minFilter = WGPUFilterMode_Linear;
    sampler_desc.magFilter = WGPUFilterMode_Linear;
    sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_desc.maxAnisotropy = 1;

    st->sampler = wgpuDeviceCreateSampler(st->device, &sampler_desc);
    if (!st->sampler) {
        return YETTY_ERR(yetty_core_void, "failed to create sampler");
    }

    /* Uniform buffer */
    WGPUBufferDescriptor buf_desc = {0};
    buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    buf_desc.size = 16;

    st->uniform_buffer = wgpuDeviceCreateBuffer(st->device, &buf_desc);
    if (!st->uniform_buffer) {
        return YETTY_ERR(yetty_core_void, "failed to create uniform buffer");
    }

    /* Placeholder texture for unused slots */
    WGPUTextureDescriptor tex_desc = {0};
    tex_desc.usage = WGPUTextureUsage_TextureBinding;
    tex_desc.dimension = WGPUTextureDimension_2D;
    tex_desc.size.width = 1;
    tex_desc.size.height = 1;
    tex_desc.size.depthOrArrayLayers = 1;
    tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;

    st->placeholder_texture = wgpuDeviceCreateTexture(st->device, &tex_desc);
    if (!st->placeholder_texture) {
        return YETTY_ERR(yetty_core_void, "failed to create placeholder texture");
    }

    st->placeholder_view = wgpuTextureCreateView(st->placeholder_texture, NULL);
    if (!st->placeholder_view) {
        return YETTY_ERR(yetty_core_void, "failed to create placeholder view");
    }

    ydebug("surface_render_target: blend pipeline created");
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result surface_render_target_render(
    struct yetty_render_target *self,
    const struct yetty_render_gpu_resource_set **resource_sets,
    size_t count)
{
    struct surface_render_target *st = (struct surface_render_target *)self;

    if (count == 0) {
        return YETTY_OK_VOID();
    }
    if (count > MAX_LAYERS) {
        yerror("surface_render_target: too many layers (%zu > %d)", count, MAX_LAYERS);
        count = MAX_LAYERS;
    }

    /* Create blend pipeline on first use */
    if (!st->blend_pipeline) {
        struct yetty_core_void_result res = create_blend_pipeline(st);
        if (!YETTY_IS_OK(res)) {
            return res;
        }
    }

    /* Render each resource_set to intermediate texture */
    struct yetty_render_rendered_layer *rendered_layers[MAX_LAYERS] = {0};

    for (size_t i = 0; i < count; i++) {
        /* Create layer_renderer if needed */
        if (!st->layer_renderers[i]) {
            struct yetty_render_layer_renderer_result lr_res =
                yetty_render_layer_renderer_surface_create(st->device, st->queue, st->format);
            if (!YETTY_IS_OK(lr_res)) {
                return YETTY_ERR(yetty_core_void, lr_res.error.msg);
            }
            st->layer_renderers[i] = lr_res.value;
        }

        /* Render to intermediate texture */
        struct yetty_render_rendered_layer_result rl_res =
            st->layer_renderers[i]->ops->render(st->layer_renderers[i], resource_sets[i]);
        if (!YETTY_IS_OK(rl_res)) {
            return YETTY_ERR(yetty_core_void, rl_res.error.msg);
        }
        rendered_layers[i] = rl_res.value;
    }

    /* Acquire surface texture */
    WGPUSurfaceTexture surface_texture;
    wgpuSurfaceGetCurrentTexture(st->surface, &surface_texture);

    if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        return YETTY_ERR(yetty_core_void, "surface not ready");
    }

    WGPUTextureView surface_view = wgpuTextureCreateView(surface_texture.texture, NULL);
    if (!surface_view) {
        return YETTY_ERR(yetty_core_void, "failed to create surface view");
    }

    /* Update uniform buffer */
    uint32_t uniforms[4] = {(uint32_t)count, 0, 0, 0};
    wgpuQueueWriteBuffer(st->queue, st->uniform_buffer, 0, uniforms, sizeof(uniforms));

    /* Build bind group with rendered layer textures */
    WGPUTextureView layer_views[MAX_LAYERS];
    for (size_t i = 0; i < MAX_LAYERS; i++) {
        if (i < count && rendered_layers[i]) {
            layer_views[i] = rendered_layers[i]->ops->get_view(rendered_layers[i]);
        } else {
            layer_views[i] = st->placeholder_view;
        }
    }

    WGPUBindGroupEntry bg_entries[6] = {0};
    for (int i = 0; i < 4; i++) {
        bg_entries[i].binding = i;
        bg_entries[i].textureView = layer_views[i];
    }
    bg_entries[4].binding = 4;
    bg_entries[4].sampler = st->sampler;
    bg_entries[5].binding = 5;
    bg_entries[5].buffer = st->uniform_buffer;
    bg_entries[5].size = 16;

    WGPUBindGroupDescriptor bg_desc = {0};
    bg_desc.layout = st->blend_layout;
    bg_desc.entryCount = 6;
    bg_desc.entries = bg_entries;

    WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(st->device, &bg_desc);
    if (!bind_group) {
        wgpuTextureViewRelease(surface_view);
        return YETTY_ERR(yetty_core_void, "failed to create bind group");
    }

    /* Blend render pass */
    WGPUCommandEncoderDescriptor enc_desc = {0};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(st->device, &enc_desc);
    if (!encoder) {
        wgpuBindGroupRelease(bind_group);
        wgpuTextureViewRelease(surface_view);
        return YETTY_ERR(yetty_core_void, "failed to create encoder");
    }

    WGPURenderPassColorAttachment color_attachment = {0};
    color_attachment.view = surface_view;
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
        wgpuTextureViewRelease(surface_view);
        return YETTY_ERR(yetty_core_void, "failed to begin render pass");
    }

    wgpuRenderPassEncoderSetPipeline(pass, st->blend_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBufferDescriptor cmd_desc = {0};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    wgpuQueueSubmit(st->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(bind_group);

    /* Present */
    wgpuSurfacePresent(st->surface);
    wgpuTextureViewRelease(surface_view);

    ydebug("surface_render_target: rendered %zu layers", count);
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result surface_render_target_resize(
    struct yetty_render_target *self,
    uint32_t width, uint32_t height)
{
    struct surface_render_target *st = (struct surface_render_target *)self;

    if ((uint32_t)st->size.width == width && (uint32_t)st->size.height == height) {
        return YETTY_OK_VOID();
    }

    st->size.width = (float)width;
    st->size.height = (float)height;

    /* Reconfigure surface */
    WGPUSurfaceConfiguration config = {0};
    config.device = st->device;
    config.format = st->format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.presentMode = WGPUPresentMode_Fifo;

    wgpuSurfaceConfigure(st->surface, &config);

    ydebug("surface_render_target_resize: %ux%u", width, height);
    return YETTY_OK_VOID();
}

static const struct yetty_render_target_ops surface_render_target_ops = {
    .destroy = surface_render_target_destroy,
    .render = surface_render_target_render,
    .resize = surface_render_target_resize,
};

struct yetty_render_target_result yetty_render_target_surface_create(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUSurface surface,
    WGPUTextureFormat format,
    uint32_t width,
    uint32_t height)
{
    struct surface_render_target *st = calloc(1, sizeof(struct surface_render_target));
    if (!st) {
        return YETTY_ERR(yetty_render_target, "failed to allocate surface target");
    }

    st->base.ops = &surface_render_target_ops;
    st->device = device;
    st->queue = queue;
    st->surface = surface;
    st->format = format;
    st->size.width = (float)width;
    st->size.height = (float)height;

    /* Configure surface */
    WGPUSurfaceConfiguration config = {0};
    config.device = device;
    config.format = format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.presentMode = WGPUPresentMode_Fifo;

    wgpuSurfaceConfigure(surface, &config);

    ydebug("yetty_render_target_surface_create: %ux%u format=%d", width, height, format);
    return YETTY_OK(yetty_render_target, &st->base);
}
