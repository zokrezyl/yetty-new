#ifndef YETTY_RENDER_GPU_RESOURCE_SET_H
#define YETTY_RENDER_GPU_RESOURCE_SET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YETTY_RENDER_GPU_RESOURCE_NAME_MAX 64
#define YETTY_RENDER_GPU_RESOURCE_WGSL_TYPE_MAX 64

/* Texture format (mirrors WGPUTextureFormat values we use) */
enum yetty_render_gpu_texture_format {
    YETTY_RENDER_GPU_TEXTURE_FORMAT_UNDEFINED = 0,
    YETTY_RENDER_GPU_TEXTURE_FORMAT_R8_UNORM = 1,
    YETTY_RENDER_GPU_TEXTURE_FORMAT_RGBA8_UNORM = 2
};

/* Sampler filter mode */
enum yetty_render_gpu_filter_mode {
    YETTY_RENDER_GPU_FILTER_NEAREST = 0,
    YETTY_RENDER_GPU_FILTER_LINEAR = 1
};

/* GPU resource set - describes resources a layer needs */
struct yetty_render_gpu_resource_set {
    int shared;  /* 1 = bind group 0, 0 = bind group 1 */
    char name[YETTY_RENDER_GPU_RESOURCE_NAME_MAX];

    /* Texture description */
    uint32_t texture_width;
    uint32_t texture_height;
    enum yetty_render_gpu_texture_format texture_format;
    char texture_wgsl_type[YETTY_RENDER_GPU_RESOURCE_WGSL_TYPE_MAX];
    char texture_name[YETTY_RENDER_GPU_RESOURCE_NAME_MAX];
    char sampler_name[YETTY_RENDER_GPU_RESOURCE_NAME_MAX];

    /* Sampler description */
    enum yetty_render_gpu_filter_mode sampler_filter;

    /* Buffer description */
    size_t buffer_size;
    char buffer_wgsl_type[YETTY_RENDER_GPU_RESOURCE_WGSL_TYPE_MAX];
    char buffer_name[YETTY_RENDER_GPU_RESOURCE_NAME_MAX];
    int buffer_readonly;

    /* Uniform buffer description */
    size_t uniform_size;
    char uniform_wgsl_type[YETTY_RENDER_GPU_RESOURCE_WGSL_TYPE_MAX];
    char uniform_name[YETTY_RENDER_GPU_RESOURCE_NAME_MAX];

    /* CPU data pointers */
    const uint8_t *texture_data;
    size_t texture_data_size;
    const uint8_t *buffer_data;
    size_t buffer_data_size;
    const uint8_t *uniform_data;
    size_t uniform_data_size;
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_GPU_RESOURCE_SET_H */
