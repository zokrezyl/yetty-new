#include <yetty/render/layer-renderer.h>
#include <yetty/render/rendered-layer.h>
#include <yetty/render/gpu-resource-binder.h>
#include <yetty/render/gpu-allocator.h>
#include <yetty/term/terminal.h>
#include <yetty/ytrace.h>
#include <stdlib.h>

struct layer_renderer_impl {
    struct yetty_render_layer_renderer base;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat format;
    uint32_t width;
    uint32_t height;

    /* Owned resources */
    struct yetty_render_gpu_allocator *allocator;
    struct yetty_render_gpu_resource_binder *binder;
    WGPUTexture intermediate_texture;
    WGPUTextureView intermediate_view;

    /* Cached rendered layer (reused when not dirty) */
    struct yetty_render_rendered_layer *cached_layer;
};

static void layer_renderer_destroy(struct yetty_render_layer_renderer *self)
{
    struct layer_renderer_impl *impl = (struct layer_renderer_impl *)self;

    if (impl->cached_layer) {
        impl->cached_layer->ops->release(impl->cached_layer);
        impl->cached_layer = NULL;
    }

    if (impl->intermediate_view) {
        wgpuTextureViewRelease(impl->intermediate_view);
        impl->intermediate_view = NULL;
    }
    if (impl->intermediate_texture) {
        wgpuTextureDestroy(impl->intermediate_texture);
        wgpuTextureRelease(impl->intermediate_texture);
        impl->intermediate_texture = NULL;
    }

    if (impl->binder) {
        impl->binder->ops->destroy(impl->binder);
        impl->binder = NULL;
    }
    if (impl->allocator) {
        impl->allocator->ops->destroy(impl->allocator);
        impl->allocator = NULL;
    }

    free(impl);
}

static void create_intermediate_texture(struct layer_renderer_impl *impl)
{
    /* Release old texture if any */
    if (impl->intermediate_view) {
        wgpuTextureViewRelease(impl->intermediate_view);
        impl->intermediate_view = NULL;
    }
    if (impl->intermediate_texture) {
        wgpuTextureDestroy(impl->intermediate_texture);
        wgpuTextureRelease(impl->intermediate_texture);
        impl->intermediate_texture = NULL;
    }

    /* Create new texture */
    WGPUTextureDescriptor tex_desc = {0};
    tex_desc.label = (WGPUStringView){.data = "layer_intermediate", .length = 18};
    tex_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    tex_desc.dimension = WGPUTextureDimension_2D;
    tex_desc.size.width = impl->width;
    tex_desc.size.height = impl->height;
    tex_desc.size.depthOrArrayLayers = 1;
    tex_desc.format = impl->format;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;

    impl->intermediate_texture = wgpuDeviceCreateTexture(impl->device, &tex_desc);
    if (!impl->intermediate_texture) {
        yerror("layer_renderer: failed to create intermediate texture");
        return;
    }

    impl->intermediate_view = wgpuTextureCreateView(impl->intermediate_texture, NULL);
    if (!impl->intermediate_view) {
        yerror("layer_renderer: failed to create intermediate view");
        wgpuTextureDestroy(impl->intermediate_texture);
        wgpuTextureRelease(impl->intermediate_texture);
        impl->intermediate_texture = NULL;
    }

    ydebug("layer_renderer: created intermediate texture %ux%u", impl->width, impl->height);
}

static struct yetty_render_rendered_layer_result layer_renderer_render(
    struct yetty_render_layer_renderer *self,
    struct yetty_term_terminal_layer *layer)
{
    struct layer_renderer_impl *impl = (struct layer_renderer_impl *)self;

    /* Ensure intermediate texture exists */
    if (!impl->intermediate_texture) {
        create_intermediate_texture(impl);
        if (!impl->intermediate_texture) {
            return YETTY_ERR(yetty_render_rendered_layer, "no intermediate texture");
        }
    }

    /* Check if layer is empty - return transparent texture, skip binder */
    if (layer->ops->is_empty && layer->ops->is_empty(layer)) {
        ydebug("layer_renderer_render: layer is empty, returning transparent texture");

        /* Clear to transparent if we don't have a cached empty layer */
        if (!impl->cached_layer) {
            /* Clear intermediate texture to transparent */
            WGPUCommandEncoderDescriptor enc_desc = {0};
            WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl->device, &enc_desc);
            if (encoder) {
                WGPURenderPassColorAttachment color_attachment = {0};
                color_attachment.view = impl->intermediate_view;
                color_attachment.loadOp = WGPULoadOp_Clear;
                color_attachment.storeOp = WGPUStoreOp_Store;
                color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 0.0};
                color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

                WGPURenderPassDescriptor pass_desc = {0};
                pass_desc.colorAttachmentCount = 1;
                pass_desc.colorAttachments = &color_attachment;

                WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
                if (pass) {
                    wgpuRenderPassEncoderEnd(pass);
                    wgpuRenderPassEncoderRelease(pass);
                }

                WGPUCommandBufferDescriptor cmd_desc = {0};
                WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
                wgpuQueueSubmit(impl->queue, 1, &cmd);
                wgpuCommandBufferRelease(cmd);
                wgpuCommandEncoderRelease(encoder);
            }

            /* Create cached layer handle */
            struct yetty_render_rendered_layer_result rl_res = yetty_render_rendered_layer_wrap(
                impl->intermediate_texture,
                impl->intermediate_view,
                impl->width,
                impl->height);
            if (YETTY_IS_OK(rl_res)) {
                impl->cached_layer = rl_res.value;
            }
            return rl_res;
        }
        return YETTY_OK(yetty_render_rendered_layer, impl->cached_layer);
    }

    /* Check if layer is dirty */
    if (!layer->dirty && impl->cached_layer) {
        ydebug("layer_renderer_render: layer not dirty, returning cached");
        return YETTY_OK(yetty_render_rendered_layer, impl->cached_layer);
    }

    /* Get GPU resource set (this clears dirty flag) */
    struct yetty_render_gpu_resource_set_result rs_res = layer->ops->get_gpu_resource_set(layer);
    if (!YETTY_IS_OK(rs_res)) {
        return YETTY_ERR(yetty_render_rendered_layer, rs_res.error.msg);
    }

    /* Submit to binder */
    struct yetty_core_void_result res = impl->binder->ops->submit(impl->binder, rs_res.value);
    if (!YETTY_IS_OK(res)) {
        return YETTY_ERR(yetty_render_rendered_layer, res.error.msg);
    }

    /* Finalize (compile shader if needed) */
    res = impl->binder->ops->finalize(impl->binder);
    if (!YETTY_IS_OK(res)) {
        return YETTY_ERR(yetty_render_rendered_layer, res.error.msg);
    }

    /* Update uniforms/buffers */
    res = impl->binder->ops->update(impl->binder);
    if (!YETTY_IS_OK(res)) {
        return YETTY_ERR(yetty_render_rendered_layer, res.error.msg);
    }

    /* Create command encoder */
    WGPUCommandEncoderDescriptor enc_desc = {0};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl->device, &enc_desc);
    if (!encoder) {
        return YETTY_ERR(yetty_render_rendered_layer, "failed to create encoder");
    }

    /* Begin render pass to intermediate texture */
    WGPURenderPassColorAttachment color_attachment = {0};
    color_attachment.view = impl->intermediate_view;
    color_attachment.loadOp = WGPULoadOp_Clear;
    color_attachment.storeOp = WGPUStoreOp_Store;
    color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 0.0};  /* transparent */
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    WGPURenderPassDescriptor pass_desc = {0};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    if (!pass) {
        wgpuCommandEncoderRelease(encoder);
        return YETTY_ERR(yetty_render_rendered_layer, "failed to begin render pass");
    }

    /* Bind and draw */
    WGPURenderPipeline pipeline = impl->binder->ops->get_pipeline(impl->binder);
    WGPUBuffer quad_vb = impl->binder->ops->get_quad_vertex_buffer(impl->binder);

    if (pipeline && quad_vb) {
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        impl->binder->ops->bind(impl->binder, pass, 0);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quad_vb, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
    } else {
        yerror("layer_renderer: missing pipeline or quad buffer");
    }

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    /* Submit */
    WGPUCommandBufferDescriptor cmd_desc = {0};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    wgpuQueueSubmit(impl->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);

    /* Release old cached layer */
    if (impl->cached_layer) {
        impl->cached_layer->ops->release(impl->cached_layer);
        impl->cached_layer = NULL;
    }

    /* Create new rendered layer handle (wraps our texture, doesn't own it) */
    struct yetty_render_rendered_layer_result rl_res = yetty_render_rendered_layer_wrap(
        impl->intermediate_texture,
        impl->intermediate_view,
        impl->width,
        impl->height);

    if (YETTY_IS_OK(rl_res)) {
        impl->cached_layer = rl_res.value;
    }

    ydebug("layer_renderer_render: rendered to intermediate texture");
    return rl_res;
}

static void layer_renderer_resize(struct yetty_render_layer_renderer *self,
                                   uint32_t width, uint32_t height)
{
    struct layer_renderer_impl *impl = (struct layer_renderer_impl *)self;

    if (width == impl->width && height == impl->height)
        return;

    impl->width = width;
    impl->height = height;

    /* Invalidate cached layer */
    if (impl->cached_layer) {
        impl->cached_layer->ops->release(impl->cached_layer);
        impl->cached_layer = NULL;
    }

    /* Recreate intermediate texture */
    create_intermediate_texture(impl);

    ydebug("layer_renderer_resize: %ux%u", width, height);
}

static const struct yetty_render_layer_renderer_ops layer_renderer_ops = {
    .destroy = layer_renderer_destroy,
    .render = layer_renderer_render,
    .resize = layer_renderer_resize,
};

struct yetty_render_layer_renderer_result yetty_render_layer_renderer_create(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUTextureFormat format,
    uint32_t width,
    uint32_t height)
{
    struct layer_renderer_impl *impl = calloc(1, sizeof(struct layer_renderer_impl));
    if (!impl)
        return YETTY_ERR(yetty_render_layer_renderer, "failed to allocate layer renderer");

    impl->base.ops = &layer_renderer_ops;
    impl->device = device;
    impl->queue = queue;
    impl->format = format;
    impl->width = width;
    impl->height = height;

    /* Create allocator */
    struct yetty_render_gpu_allocator_result alloc_res = yetty_render_gpu_allocator_create(device);
    if (!YETTY_IS_OK(alloc_res)) {
        free(impl);
        return YETTY_ERR(yetty_render_layer_renderer, alloc_res.error.msg);
    }
    impl->allocator = alloc_res.value;

    /* Create binder */
    struct yetty_render_gpu_resource_binder_result binder_res =
        yetty_render_gpu_resource_binder_create(device, queue, format, impl->allocator);
    if (!YETTY_IS_OK(binder_res)) {
        impl->allocator->ops->destroy(impl->allocator);
        free(impl);
        return YETTY_ERR(yetty_render_layer_renderer, binder_res.error.msg);
    }
    impl->binder = binder_res.value;

    /* Create intermediate texture */
    create_intermediate_texture(impl);

    ydebug("layer_renderer_create: %ux%u format=%d", width, height, format);
    return YETTY_OK(yetty_render_layer_renderer, &impl->base);
}
