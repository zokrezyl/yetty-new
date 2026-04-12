#include <yetty/render/rendered-layer.h>
#include <stdlib.h>

/* Handle that references a texture view owned by layer_renderer */
struct rendered_layer_handle {
    struct yetty_render_rendered_layer base;
    WGPUTextureView view;   /* borrowed from layer_renderer, not owned */
    uint32_t width;
    uint32_t height;
};

static void handle_release(struct yetty_render_rendered_layer *self)
{
    free(self);  /* just free the handle, texture/view owned by renderer */
}

static WGPUTextureView handle_get_view(const struct yetty_render_rendered_layer *self)
{
    const struct rendered_layer_handle *h = (const struct rendered_layer_handle *)self;
    return h->view;
}

static uint32_t handle_get_width(const struct yetty_render_rendered_layer *self)
{
    const struct rendered_layer_handle *h = (const struct rendered_layer_handle *)self;
    return h->width;
}

static uint32_t handle_get_height(const struct yetty_render_rendered_layer *self)
{
    const struct rendered_layer_handle *h = (const struct rendered_layer_handle *)self;
    return h->height;
}

static const struct yetty_render_rendered_layer_ops handle_ops = {
    .release = handle_release,
    .get_view = handle_get_view,
    .get_width = handle_get_width,
    .get_height = handle_get_height,
};

struct yetty_render_rendered_layer_result yetty_render_rendered_layer_wrap(
    WGPUTexture texture,
    WGPUTextureView view,
    uint32_t width,
    uint32_t height)
{
    (void)texture;  /* not used, view is what blender needs */

    struct rendered_layer_handle *h = calloc(1, sizeof(struct rendered_layer_handle));
    if (!h)
        return YETTY_ERR(yetty_render_rendered_layer, "failed to allocate handle");

    h->base.ops = &handle_ops;
    h->view = view;
    h->width = width;
    h->height = height;

    return YETTY_OK(yetty_render_rendered_layer, &h->base);
}
