// Auto-generated from yplot.yaml - DO NOT EDIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YETTY_YPLOT_TYPE_ID 0x80000003u
#define YETTY_YPLOT_UNIFORMS_SIZE 14
#define YETTY_YPLOT_HEADER_SIZE 15

// Uniform offsets (words from payload start)
#define YETTY_YPLOT_BOUNDS_X_OFFSET 0
#define YETTY_YPLOT_BOUNDS_Y_OFFSET 1
#define YETTY_YPLOT_BOUNDS_W_OFFSET 2
#define YETTY_YPLOT_BOUNDS_H_OFFSET 3
#define YETTY_YPLOT_FLAGS_OFFSET 4
#define YETTY_YPLOT_FUNCTION_COUNT_OFFSET 5
#define YETTY_YPLOT_COLORS_OFFSET 6

// Buffer length offsets
#define YETTY_YPLOT_BYTECODE_LEN_OFFSET 14

// Uniforms struct
struct yetty_yplot_uniforms {
    float bounds_x;
    float bounds_y;
    float bounds_w;
    float bounds_h;
    uint32_t flags;
    uint32_t function_count;
    uint32_t colors[8];
};

// Serialization
size_t yetty_yplot_serialized_size(
    const struct yetty_yplot_uniforms *uniforms,
    const uint32_t *bytecode, size_t bytecode_len);

struct yetty_core_size_result yetty_yplot_serialize(
    const struct yetty_yplot_uniforms *uniforms,
    const uint32_t *bytecode, size_t bytecode_len,
    uint8_t *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif
