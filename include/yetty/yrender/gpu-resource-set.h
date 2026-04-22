#ifndef YETTY_RENDER_GPU_RESOURCE_SET_H
#define YETTY_RENDER_GPU_RESOURCE_SET_H

#include <yetty/yrender/types.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YETTY_RENDER_RS_MAX_TEXTURES  4
#define YETTY_RENDER_RS_MAX_BUFFERS   4
#define YETTY_RENDER_RS_MAX_UNIFORMS 16
#define YETTY_RENDER_RS_MAX_CHILDREN  4

/* GPU resource set - collection of resources a provider needs */
struct yetty_yrender_gpu_resource_set {
    char namespace[YETTY_RENDER_NAME_MAX];
    struct pixel_size pixel_size;

    struct yetty_render_texture textures[YETTY_RENDER_RS_MAX_TEXTURES];
    size_t texture_count;

    struct yetty_render_buffer buffers[YETTY_RENDER_RS_MAX_BUFFERS];
    size_t buffer_count;

    struct yetty_render_uniform uniforms[YETTY_RENDER_RS_MAX_UNIFORMS];
    size_t uniform_count;

    struct yetty_render_shader_code shader;

    struct yetty_yrender_gpu_resource_set *children[YETTY_RENDER_RS_MAX_CHILDREN];
    size_t children_count;
};

YETTY_RESULT_DECLARE(yetty_yrender_gpu_resource_set, const struct yetty_yrender_gpu_resource_set *);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_GPU_RESOURCE_SET_H */
