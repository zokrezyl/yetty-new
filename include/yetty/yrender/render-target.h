#ifndef YETTY_RENDER_TARGET_H
#define YETTY_RENDER_TARGET_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_target;
struct yetty_yterm_terminal_layer;
struct yetty_render_gpu_allocator;
struct yetty_gpu_context;

/* Result type */
YETTY_RESULT_DECLARE(yetty_render_target_ptr, struct yetty_render_target *);

/* Viewport - position and size */
struct yetty_render_viewport {
	float x, y, w, h;
};

/*=============================================================================
 * Render target - unified abstraction for rendering
 *
 * Used at multiple levels:
 * - Layer level: render_layer() renders a terminal layer to texture
 * - Terminal level: blend() composites layer targets together
 * - Yetty level: blend() composites terminal targets with viewport/scissor
 * - Final level: present() outputs to surface/vnc/ymux
 *===========================================================================*/

struct yetty_render_target_ops {
	void (*destroy)(struct yetty_render_target *self);

	/* Clear the target to a solid color */
	struct yetty_core_void_result (*clear)(
		struct yetty_render_target *self);

	/* Render single terminal layer to this target */
	struct yetty_core_void_result (*render_layer)(
		struct yetty_render_target *self,
		struct yetty_yterm_terminal_layer *layer);

	/* Blend multiple source targets into this target */
	struct yetty_core_void_result (*blend)(
		struct yetty_render_target *self,
		struct yetty_render_target **sources,
		size_t count);

	/* Present this target's content to final destination (surface/vnc/ymux) */
	struct yetty_core_void_result (*present)(
		struct yetty_render_target *self);

	/* Get texture view for blending */
	WGPUTextureView (*get_view)(const struct yetty_render_target *self);

	/* Resize/reposition the target */
	struct yetty_core_void_result (*resize)(
		struct yetty_render_target *self,
		struct yetty_render_viewport viewport);
};

/* Render target base - embed as first member in subclasses */
struct yetty_render_target {
	const struct yetty_render_target_ops *ops;
	struct yetty_render_viewport viewport;
};

/*=============================================================================
 * Texture render target - renders to GPU texture
 *
 * Used for:
 * - Layer targets (render_layer) - surface=NULL
 * - Terminal compositing target (blend layers) - surface=NULL
 * - Big yetty texture (blend terminals + present) - surface provided
 *
 * If surface is provided, present() blits to it.
 * If surface is NULL, present() returns error.
 *===========================================================================*/

struct yetty_render_target_ptr_result yetty_render_target_texture_create(
	WGPUDevice device,
	WGPUQueue queue,
	WGPUTextureFormat format,
	struct yetty_render_gpu_allocator *allocator,
	WGPUSurface surface,  /* NULL for layer/terminal targets */
	struct yetty_render_viewport viewport);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_TARGET_H */
