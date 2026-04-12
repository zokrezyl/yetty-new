#ifndef YETTY_RENDER_TARGET_H
#define YETTY_RENDER_TARGET_H

#include <stdint.h>
#include <yetty/core/result.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_target;

/* Result type */
YETTY_RESULT_DECLARE(yetty_render_target, struct yetty_render_target *);

/* Render target ops */
struct yetty_render_target_ops {
    void (*destroy)(struct yetty_render_target *self);

    /* Acquire texture view for rendering. Returns NULL on error. */
    WGPUTextureView (*acquire)(struct yetty_render_target *self);

    /* Present/commit the rendered frame */
    void (*present)(struct yetty_render_target *self);

    /* Resize the target */
    void (*resize)(struct yetty_render_target *self, uint32_t width, uint32_t height);

    /* Dimensions */
    uint32_t (*get_width)(const struct yetty_render_target *self);
    uint32_t (*get_height)(const struct yetty_render_target *self);

    /* Format */
    WGPUTextureFormat (*get_format)(const struct yetty_render_target *self);
};

/* Render target base */
struct yetty_render_target {
    const struct yetty_render_target_ops *ops;
};

/* Create surface target (local window) */
struct yetty_render_target_result yetty_render_target_surface_create(
    WGPUDevice device,
    WGPUSurface surface,
    WGPUTextureFormat format,
    uint32_t width,
    uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_TARGET_H */
