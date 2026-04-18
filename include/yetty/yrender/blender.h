#ifndef YETTY_RENDER_BLENDER_H
#define YETTY_RENDER_BLENDER_H

#include <stddef.h>
#include <yetty/ycore/result.h>
#include <yetty/yrender/render-target.h>
#include <yetty/yrender/rendered-layer.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_blender;

/* Result type */
YETTY_RESULT_DECLARE(yetty_render_blender, struct yetty_render_blender *);

/* Blender ops */
struct yetty_render_blender_ops {
    void (*destroy)(struct yetty_render_blender *self);

    /* Blend rendered layers to target */
    struct yetty_core_void_result (*blend)(
        struct yetty_render_blender *self,
        struct yetty_render_rendered_layer **layers,
        size_t layer_count);

    /* Set/replace render target (takes ownership, destroys old target) */
    void (*set_target)(
        struct yetty_render_blender *self,
        struct yetty_render_target *target);

    /* Get current target (not owned by caller) */
    struct yetty_render_target *(*get_target)(
        const struct yetty_render_blender *self);
};

/* Blender base */
struct yetty_render_blender {
    const struct yetty_render_blender_ops *ops;
};

/* Create blender with initial target (takes ownership of target) */
struct yetty_render_blender_result yetty_render_blender_create(
    WGPUDevice device,
    WGPUQueue queue,
    struct yetty_render_target *target,
    const char *shader_path);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_BLENDER_H */
