/*
 * layer-renderer-surface.c - Surface specialization of layer_renderer
 *
 * layer_renderer is an abstract interface for rendering gpu_resource_sets.
 * This "surface" specialization renders to an intermediate GPU texture using
 * WebGPU binder/allocator. The ymux specialization serializes for remote.
 */

#include <yetty/yrender/layer-renderer.h>
#include <yetty/yrender/rendered-layer.h>
#include <yetty/yrender/gpu-resource-binder.h>
#include <yetty/yrender/gpu-allocator.h>
#include <yetty/ytrace.h>
#include <stdbool.h>
#include <stdlib.h>

struct layer_renderer_surface {
    struct yetty_render_layer_renderer base;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat format;

    /* Current intermediate texture size (recreated if resource_set size differs) */
    struct pixel_size tex_size;

    /* Owned resources */
    struct yetty_render_gpu_allocator *allocator;
    struct yetty_render_gpu_resource_binder *binder;
    WGPUTexture intermediate_texture;
    WGPUTextureView intermediate_view;

    /* Cached rendered layer (reused when not dirty) */
    struct yetty_render_rendered_layer *cached_layer;
};

static void layer_renderer_surface_destroy(struct yetty_render_layer_renderer *self)
{
    struct layer_renderer_surface *lr = (struct layer_renderer_surface *)self;

    if (lr->cached_layer) {
        lr->cached_layer->ops->release(lr->cached_layer);
        lr->cached_layer = NULL;
    }

    if (lr->intermediate_view) {
        wgpuTextureViewRelease(lr->intermediate_view);
        lr->intermediate_view = NULL;
    }
    if (lr->intermediate_texture) {
        wgpuTextureDestroy(lr->intermediate_texture);
        wgpuTextureRelease(lr->intermediate_texture);
        lr->intermediate_texture = NULL;
    }

    if (lr->binder) {
        lr->binder->ops->destroy(lr->binder);
        lr->binder = NULL;
    }
    if (lr->allocator) {
        lr->allocator->ops->destroy(lr->allocator);
        lr->allocator = NULL;
    }

    free(lr);
}

static void create_intermediate_texture(struct layer_renderer_surface *lr,
                                        struct pixel_size size)
{
    /* Release old texture if any */
    if (lr->intermediate_view) {
        wgpuTextureViewRelease(lr->intermediate_view);
        lr->intermediate_view = NULL;
    }
    if (lr->intermediate_texture) {
        wgpuTextureDestroy(lr->intermediate_texture);
        wgpuTextureRelease(lr->intermediate_texture);
        lr->intermediate_texture = NULL;
    }

    lr->tex_size = size;

    /* Create new texture */
    WGPUTextureDescriptor tex_desc = {0};
    tex_desc.label = (WGPUStringView){.data = "layer_intermediate", .length = 18};
    tex_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    tex_desc.dimension = WGPUTextureDimension_2D;
    tex_desc.size.width = (uint32_t)size.width;
    tex_desc.size.height = (uint32_t)size.height;
    tex_desc.size.depthOrArrayLayers = 1;
    tex_desc.format = lr->format;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;

    lr->intermediate_texture = wgpuDeviceCreateTexture(lr->device, &tex_desc);
    if (!lr->intermediate_texture) {
        yerror("layer_renderer_surface: failed to create intermediate texture");
        return;
    }

    lr->intermediate_view = wgpuTextureCreateView(lr->intermediate_texture, NULL);
    if (!lr->intermediate_view) {
        yerror("layer_renderer_surface: failed to create intermediate view");
        wgpuTextureDestroy(lr->intermediate_texture);
        wgpuTextureRelease(lr->intermediate_texture);
        lr->intermediate_texture = NULL;
    }

    ydebug("layer_renderer_surface: created intermediate texture %.0fx%.0f",
           size.width, size.height);
}

static bool size_changed(struct pixel_size a, struct pixel_size b)
{
    return (uint32_t)a.width != (uint32_t)b.width ||
           (uint32_t)a.height != (uint32_t)b.height;
}

static struct yetty_render_rendered_layer_result layer_renderer_surface_render(
    struct yetty_render_layer_renderer *self,
    const struct yetty_render_gpu_resource_set *rs)
{
    struct layer_renderer_surface *lr = (struct layer_renderer_surface *)self;

    /* Recreate intermediate texture if size changed */
    if (!lr->intermediate_texture || size_changed(lr->tex_size, rs->pixel_size)) {
        create_intermediate_texture(lr, rs->pixel_size);
        if (!lr->intermediate_texture) {
            return YETTY_ERR(yetty_render_rendered_layer, "no intermediate texture");
        }
        /* Invalidate cache on resize */
        if (lr->cached_layer) {
            lr->cached_layer->ops->release(lr->cached_layer);
            lr->cached_layer = NULL;
        }
    }

    /* Submit to binder */
    struct yetty_core_void_result res = lr->binder->ops->submit(lr->binder, rs);
    if (!YETTY_IS_OK(res)) {
        return YETTY_ERR(yetty_render_rendered_layer, res.error.msg);
    }

    /* Finalize (compile shader if needed) */
    res = lr->binder->ops->finalize(lr->binder);
    if (!YETTY_IS_OK(res)) {
        return YETTY_ERR(yetty_render_rendered_layer, res.error.msg);
    }

    /* Update uniforms/buffers */
    res = lr->binder->ops->update(lr->binder);
    if (!YETTY_IS_OK(res)) {
        return YETTY_ERR(yetty_render_rendered_layer, res.error.msg);
    }

    /* Create command encoder */
    WGPUCommandEncoderDescriptor enc_desc = {0};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(lr->device, &enc_desc);
    if (!encoder) {
        return YETTY_ERR(yetty_render_rendered_layer, "failed to create encoder");
    }

    /* Begin render pass to intermediate texture */
    WGPURenderPassColorAttachment color_attachment = {0};
    color_attachment.view = lr->intermediate_view;
    color_attachment.loadOp = WGPULoadOp_Clear;
    color_attachment.storeOp = WGPUStoreOp_Store;
    color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 0.0};
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
    WGPURenderPipeline pipeline = lr->binder->ops->get_pipeline(lr->binder);
    WGPUBuffer quad_vb = lr->binder->ops->get_quad_vertex_buffer(lr->binder);

    if (pipeline && quad_vb) {
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        lr->binder->ops->bind(lr->binder, pass, 0);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quad_vb, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
    } else {
        yerror("layer_renderer_surface: missing pipeline or quad buffer");
    }

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    /* Submit */
    WGPUCommandBufferDescriptor cmd_desc = {0};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    wgpuQueueSubmit(lr->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);

    /* Release old cached layer */
    if (lr->cached_layer) {
        lr->cached_layer->ops->release(lr->cached_layer);
        lr->cached_layer = NULL;
    }

    /* Create new rendered layer handle (wraps our texture, doesn't own it) */
    struct yetty_render_rendered_layer_result rl_res = yetty_render_rendered_layer_wrap(
        lr->intermediate_texture,
        lr->intermediate_view,
        (uint32_t)lr->tex_size.width,
        (uint32_t)lr->tex_size.height);

    if (YETTY_IS_OK(rl_res)) {
        lr->cached_layer = rl_res.value;
    }

    ydebug("layer_renderer_surface: rendered to intermediate texture");
    return rl_res;
}

static const struct yetty_render_layer_renderer_ops layer_renderer_surface_ops = {
    .destroy = layer_renderer_surface_destroy,
    .render = layer_renderer_surface_render,
};

struct yetty_render_layer_renderer_result yetty_render_layer_renderer_surface_create(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUTextureFormat format)
{
    struct layer_renderer_surface *lr = calloc(1, sizeof(struct layer_renderer_surface));
    if (!lr)
        return YETTY_ERR(yetty_render_layer_renderer, "failed to allocate layer renderer");

    lr->base.ops = &layer_renderer_surface_ops;
    lr->device = device;
    lr->queue = queue;
    lr->format = format;

    /* Create allocator */
    struct yetty_render_gpu_allocator_result alloc_res = yetty_render_gpu_allocator_create(device);
    if (!YETTY_IS_OK(alloc_res)) {
        free(lr);
        return YETTY_ERR(yetty_render_layer_renderer, alloc_res.error.msg);
    }
    lr->allocator = alloc_res.value;

    /* Create binder */
    struct yetty_render_gpu_resource_binder_result binder_res =
        yetty_render_gpu_resource_binder_create(device, queue, format, lr->allocator);
    if (!YETTY_IS_OK(binder_res)) {
        lr->allocator->ops->destroy(lr->allocator);
        free(lr);
        return YETTY_ERR(yetty_render_layer_renderer, binder_res.error.msg);
    }
    lr->binder = binder_res.value;

    /* Intermediate texture created on first render when we know the size */

    ydebug("layer_renderer_surface_create: format=%d", format);
    return YETTY_OK(yetty_render_layer_renderer, &lr->base);
}
