#include <yetty/render/rendered-layer.h>
#include <yetty/ytrace.h>
#include <stdlib.h>

struct rendered_layer_impl {
    struct yetty_render_rendered_layer base;
    WGPUTexture texture;
    WGPUTextureView view;
    uint32_t width;
    uint32_t height;
    int owned;  /* whether we own the texture */
};

static void rendered_layer_release(struct yetty_render_rendered_layer *self)
{
    struct rendered_layer_impl *impl = (struct rendered_layer_impl *)self;

    if (impl->view) {
        wgpuTextureViewRelease(impl->view);
        impl->view = NULL;
    }
    if (impl->owned && impl->texture) {
        wgpuTextureDestroy(impl->texture);
        wgpuTextureRelease(impl->texture);
        impl->texture = NULL;
    }
    free(impl);
}

static WGPUTextureView rendered_layer_get_view(const struct yetty_render_rendered_layer *self)
{
    const struct rendered_layer_impl *impl = (const struct rendered_layer_impl *)self;
    return impl->view;
}

static uint32_t rendered_layer_get_width(const struct yetty_render_rendered_layer *self)
{
    const struct rendered_layer_impl *impl = (const struct rendered_layer_impl *)self;
    return impl->width;
}

static uint32_t rendered_layer_get_height(const struct yetty_render_rendered_layer *self)
{
    const struct rendered_layer_impl *impl = (const struct rendered_layer_impl *)self;
    return impl->height;
}

static const struct yetty_render_rendered_layer_ops rendered_layer_ops = {
    .release = rendered_layer_release,
    .get_view = rendered_layer_get_view,
    .get_width = rendered_layer_get_width,
    .get_height = rendered_layer_get_height,
};

/* Create rendered layer wrapping existing texture (not owned) */
struct yetty_render_rendered_layer_result yetty_render_rendered_layer_wrap(
    WGPUTexture texture,
    WGPUTextureView view,
    uint32_t width,
    uint32_t height)
{
    struct rendered_layer_impl *impl = calloc(1, sizeof(struct rendered_layer_impl));
    if (!impl)
        return YETTY_ERR(yetty_render_rendered_layer, "failed to allocate rendered layer");

    impl->base.ops = &rendered_layer_ops;
    impl->texture = texture;
    impl->view = view;
    impl->width = width;
    impl->height = height;
    impl->owned = 0;

    return YETTY_OK(yetty_render_rendered_layer, &impl->base);
}
