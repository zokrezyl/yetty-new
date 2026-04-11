#ifndef YETTY_RENDER_TYPES_H
#define YETTY_RENDER_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YETTY_RENDER_NAME_MAX 64
#define YETTY_RENDER_WGSL_TYPE_MAX 64

/* Storage buffer */
struct yetty_render_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    char name[YETTY_RENDER_NAME_MAX];
    char wgsl_type[YETTY_RENDER_WGSL_TYPE_MAX];
    int readonly;
    int dirty;
};

/* Texture */
struct yetty_render_texture {
    uint8_t *data;
    uint32_t width;
    uint32_t height;
    uint32_t format;          /* WGPUTextureFormat */
    char name[YETTY_RENDER_NAME_MAX];
    char wgsl_type[YETTY_RENDER_WGSL_TYPE_MAX];
    char sampler_name[YETTY_RENDER_NAME_MAX];
    uint32_t sampler_filter;  /* WGPUFilterMode */
    int dirty;
};

/* Uniform value types */
enum yetty_render_uniform_type {
    YETTY_RENDER_UNIFORM_F32,
    YETTY_RENDER_UNIFORM_VEC2,
    YETTY_RENDER_UNIFORM_VEC3,
    YETTY_RENDER_UNIFORM_VEC4,
    YETTY_RENDER_UNIFORM_MAT4,
    YETTY_RENDER_UNIFORM_U32,
    YETTY_RENDER_UNIFORM_I32,
};

/* Uniform: named typed scalar/vector/matrix value */
struct yetty_render_uniform {
    char name[YETTY_RENDER_NAME_MAX];
    enum yetty_render_uniform_type type;
    union {
        float f32;
        float vec2[2];
        float vec3[3];
        float vec4[4];
        float mat4[16];
        uint32_t u32;
        int32_t i32;
    };
};

/* Shader code with precomputed hash */
struct yetty_render_shader_code {
    const char *data;
    size_t size;
    uint64_t hash;
};

/* Set shader code and compute hash */
void yetty_render_shader_code_set(struct yetty_render_shader_code *sc,
                                   const char *data, size_t size);

/* Compute FNV-1a hash */
uint64_t yetty_render_hash(const void *data, size_t size);

/* Returns WGSL type string for a uniform type */
const char *yetty_render_uniform_type_wgsl(enum yetty_render_uniform_type type);

/* Returns byte size of a uniform type */
size_t yetty_render_uniform_type_size(enum yetty_render_uniform_type type);

/* Returns WGSL alignment requirement for a uniform type */
size_t yetty_render_uniform_type_align(enum yetty_render_uniform_type type);

/* Returns total byte size of texture pixel data */
size_t yetty_render_texture_get_size(const struct yetty_render_texture *texture);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_RENDER_TYPES_H */
