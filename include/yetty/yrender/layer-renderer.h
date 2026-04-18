#ifndef YETTY_RENDER_LAYER_RENDERER_H
#define YETTY_RENDER_LAYER_RENDERER_H

#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/yrender/rendered-layer.h>
#include <yetty/yterm/terminal.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_layer_renderer;

/* Result type */
YETTY_RESULT_DECLARE(yetty_render_layer_renderer, struct yetty_render_layer_renderer *);

/* Layer renderer ops */
struct yetty_render_layer_renderer_ops {
    void (*destroy)(struct yetty_render_layer_renderer *self);

    /* Render a terminal layer to texture, returns rendered_layer handle */
    struct yetty_render_rendered_layer_result (*render)(
        struct yetty_render_layer_renderer *self,
        struct yetty_term_terminal_layer *layer);

    /* Resize intermediate textures */
    void (*resize)(struct yetty_render_layer_renderer *self,
                   uint32_t width, uint32_t height);
};

/* Layer renderer base */
struct yetty_render_layer_renderer {
    const struct yetty_render_layer_renderer_ops *ops;
};

/* Create layer renderer */
struct yetty_render_layer_renderer_result yetty_render_layer_renderer_create(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUTextureFormat format,
    uint32_t width,
    uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_LAYER_RENDERER_H */
