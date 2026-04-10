#ifndef YETTY_RENDER_GPU_ALLOCATOR_H
#define YETTY_RENDER_GPU_ALLOCATOR_H

#include <yetty/core/result.h>
#include <webgpu/webgpu.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_gpu_allocator;

struct yetty_render_gpu_allocator_ops {
    void (*destroy)(struct yetty_render_gpu_allocator *self);
    WGPUBuffer (*create_buffer)(struct yetty_render_gpu_allocator *self,
                                 const WGPUBufferDescriptor *desc);
    void (*release_buffer)(struct yetty_render_gpu_allocator *self, WGPUBuffer buffer);
    WGPUTexture (*create_texture)(struct yetty_render_gpu_allocator *self,
                                   const WGPUTextureDescriptor *desc);
    void (*release_texture)(struct yetty_render_gpu_allocator *self, WGPUTexture texture);
    uint64_t (*total_allocated_bytes)(const struct yetty_render_gpu_allocator *self);
};

struct yetty_render_gpu_allocator {
    const struct yetty_render_gpu_allocator_ops *ops;
};

YETTY_RESULT_DECLARE(yetty_render_gpu_allocator, struct yetty_render_gpu_allocator *);

struct yetty_render_gpu_allocator_result yetty_render_gpu_allocator_create(WGPUDevice device);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_GPU_ALLOCATOR_H */
