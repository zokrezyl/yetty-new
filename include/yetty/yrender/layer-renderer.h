#ifndef YETTY_RENDER_LAYER_RENDERER_H
#define YETTY_RENDER_LAYER_RENDERER_H

#include <yetty/ycore/result.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/yrender/rendered-layer.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_layer_renderer;

/* Result type */
YETTY_RESULT_DECLARE(yetty_render_layer_renderer,
		     struct yetty_render_layer_renderer *);

/* Layer renderer vtable */
struct yetty_render_layer_renderer_ops {
	void (*destroy)(struct yetty_render_layer_renderer *self);

	struct yetty_render_rendered_layer_result (*render)(
		struct yetty_render_layer_renderer *self,
		const struct yetty_render_gpu_resource_set *resource_set);
};

/* Layer renderer base - embed as first member in implementations */
struct yetty_render_layer_renderer {
	const struct yetty_render_layer_renderer_ops *ops;
};

/* Surface specialization: renders to GPU intermediate texture via binder */
struct yetty_render_layer_renderer_result yetty_render_layer_renderer_surface_create(
	WGPUDevice device, WGPUQueue queue, WGPUTextureFormat format);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_LAYER_RENDERER_H */
