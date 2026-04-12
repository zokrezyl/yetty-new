#ifndef YETTY_RENDER_RENDERED_LAYER_H
#define YETTY_RENDER_RENDERED_LAYER_H

#include <stdint.h>
#include <yetty/core/result.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_rendered_layer;

/* Result type */
YETTY_RESULT_DECLARE(yetty_render_rendered_layer, struct yetty_render_rendered_layer *);

/* Rendered layer ops */
struct yetty_render_rendered_layer_ops {
    /* Release the rendered layer (returns texture to pool) */
    void (*release)(struct yetty_render_rendered_layer *self);

    /* Get texture view for compositing */
    WGPUTextureView (*get_view)(const struct yetty_render_rendered_layer *self);

    /* Dimensions */
    uint32_t (*get_width)(const struct yetty_render_rendered_layer *self);
    uint32_t (*get_height)(const struct yetty_render_rendered_layer *self);
};

/* Rendered layer base */
struct yetty_render_rendered_layer {
    const struct yetty_render_rendered_layer_ops *ops;
};

/* Create rendered layer wrapping existing texture (view not owned) */
struct yetty_render_rendered_layer_result yetty_render_rendered_layer_wrap(
    WGPUTexture texture,
    WGPUTextureView view,
    uint32_t width,
    uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_RENDERED_LAYER_H */
