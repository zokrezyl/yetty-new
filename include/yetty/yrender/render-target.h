#ifndef YETTY_RENDER_TARGET_H
#define YETTY_RENDER_TARGET_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/yrender/gpu-resource-set.h>
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

	/* Render gpu resource sets */
	struct yetty_core_void_result (*render)(
		struct yetty_render_target *self,
		const struct yetty_render_gpu_resource_set **resource_sets,
		size_t count);

	/* Resize the target */
	struct yetty_core_void_result (*resize)(struct yetty_render_target *self,
						uint32_t width, uint32_t height);
};

/* Render target base */
struct yetty_render_target {
	const struct yetty_render_target_ops *ops;
};

/* Create surface render target (contains binder + blender internally) */
struct yetty_render_target_result yetty_render_target_surface_create(
	WGPUDevice device, WGPUQueue queue, WGPUSurface surface,
	WGPUTextureFormat format, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_TARGET_H */
