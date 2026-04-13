#include <yetty/yrender/render-target.h>
#include <yetty/ytrace.h>
#include <stdlib.h>

struct surface_target {
    struct yetty_render_target base;
    WGPUDevice device;
    WGPUSurface surface;
    WGPUTextureFormat format;
    uint32_t width;
    uint32_t height;
    WGPUTexture current_texture;
    WGPUTextureView current_view;
};

static void surface_target_destroy(struct yetty_render_target *self)
{
    struct surface_target *st = (struct surface_target *)self;

    if (st->current_view) {
        wgpuTextureViewRelease(st->current_view);
    }
    /* Note: surface texture is owned by surface, don't release it */

    free(st);
}

static WGPUTextureView surface_target_acquire(struct yetty_render_target *self)
{
    struct surface_target *st = (struct surface_target *)self;

    /* Release previous view if any */
    if (st->current_view) {
        wgpuTextureViewRelease(st->current_view);
        st->current_view = NULL;
    }

    /* Get current surface texture */
    WGPUSurfaceTexture surface_texture;
    wgpuSurfaceGetCurrentTexture(st->surface, &surface_texture);

    if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        yerror("surface_target_acquire: surface texture not ready, status=%d",
               surface_texture.status);
        return NULL;
    }

    st->current_texture = surface_texture.texture;
    st->current_view = wgpuTextureCreateView(st->current_texture, NULL);

    return st->current_view;
}

static void surface_target_present(struct yetty_render_target *self)
{
    struct surface_target *st = (struct surface_target *)self;

    wgpuSurfacePresent(st->surface);

    /* Release view after present */
    if (st->current_view) {
        wgpuTextureViewRelease(st->current_view);
        st->current_view = NULL;
    }
    st->current_texture = NULL;
}

static void surface_target_resize(struct yetty_render_target *self,
                                   uint32_t width, uint32_t height)
{
    struct surface_target *st = (struct surface_target *)self;

    if (width == st->width && height == st->height)
        return;

    st->width = width;
    st->height = height;

    /* Reconfigure surface */
    WGPUSurfaceConfiguration config = {0};
    config.device = st->device;
    config.format = st->format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.presentMode = WGPUPresentMode_Fifo;

    wgpuSurfaceConfigure(st->surface, &config);
    ydebug("surface_target_resize: %ux%u", width, height);
}

static uint32_t surface_target_get_width(const struct yetty_render_target *self)
{
    const struct surface_target *st = (const struct surface_target *)self;
    return st->width;
}

static uint32_t surface_target_get_height(const struct yetty_render_target *self)
{
    const struct surface_target *st = (const struct surface_target *)self;
    return st->height;
}

static WGPUTextureFormat surface_target_get_format(const struct yetty_render_target *self)
{
    const struct surface_target *st = (const struct surface_target *)self;
    return st->format;
}

static const struct yetty_render_target_ops surface_target_ops = {
    .destroy = surface_target_destroy,
    .acquire = surface_target_acquire,
    .present = surface_target_present,
    .resize = surface_target_resize,
    .get_width = surface_target_get_width,
    .get_height = surface_target_get_height,
    .get_format = surface_target_get_format,
};

struct yetty_render_target_result yetty_render_target_surface_create(
    WGPUDevice device,
    WGPUSurface surface,
    WGPUTextureFormat format,
    uint32_t width,
    uint32_t height)
{
    struct surface_target *st = calloc(1, sizeof(struct surface_target));
    if (!st)
        return YETTY_ERR(yetty_render_target, "failed to allocate surface target");

    st->base.ops = &surface_target_ops;
    st->device = device;
    st->surface = surface;
    st->format = format;
    st->width = width;
    st->height = height;

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
