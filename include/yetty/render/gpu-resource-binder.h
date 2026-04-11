#ifndef YETTY_RENDER_GPU_RESOURCE_BINDER_H
#define YETTY_RENDER_GPU_RESOURCE_BINDER_H

#include <yetty/core/result.h>
#include <yetty/render/gpu-resource-set.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_gpu_resource_binder;
struct yetty_render_gpu_allocator;

struct yetty_render_gpu_resource_binder_ops {
    void (*destroy)(struct yetty_render_gpu_resource_binder *self);
    struct yetty_core_void_result (*submit)(struct yetty_render_gpu_resource_binder *self,
                                             const struct yetty_render_gpu_resource_set *rs);
    struct yetty_core_void_result (*finalize)(struct yetty_render_gpu_resource_binder *self);
    struct yetty_core_void_result (*update)(struct yetty_render_gpu_resource_binder *self);
    struct yetty_core_void_result (*bind)(struct yetty_render_gpu_resource_binder *self,
                                           WGPURenderPassEncoder pass, uint32_t group_index);
    WGPURenderPipeline (*get_pipeline)(const struct yetty_render_gpu_resource_binder *self);
    WGPUBuffer (*get_quad_vertex_buffer)(const struct yetty_render_gpu_resource_binder *self);
};

struct yetty_render_gpu_resource_binder {
    const struct yetty_render_gpu_resource_binder_ops *ops;
};

YETTY_RESULT_DECLARE(yetty_render_gpu_resource_binder, struct yetty_render_gpu_resource_binder *);

struct yetty_render_gpu_resource_binder_result yetty_render_gpu_resource_binder_create(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUTextureFormat surface_format,
    struct yetty_render_gpu_allocator *allocator);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_GPU_RESOURCE_BINDER_H */
