// Auto-generated from yplot.yaml - DO NOT EDIT

#include "yplot-gen.h"
#include <string.h>

size_t yetty_yplot_serialized_size(
    const struct yetty_yplot_uniforms *uniforms,
    const uint32_t *bytecode, size_t bytecode_len)
{
    (void)uniforms;
    // FAM header (2) + uniforms + buffer lengths + buffer data
    return (2 + YETTY_YPLOT_HEADER_SIZE + bytecode_len) * sizeof(uint32_t);
}

struct yetty_core_size_result yetty_yplot_serialize(
    const struct yetty_yplot_uniforms *uniforms,
    const uint32_t *bytecode, size_t bytecode_len,
    uint8_t *out, size_t out_capacity)
{
    if (!uniforms)
        return YETTY_ERR(yetty_core_size, "uniforms is NULL");
    if (!out)
        return YETTY_ERR(yetty_core_size, "out is NULL");

    size_t total_buf_words = bytecode_len;
    size_t required = (2 + YETTY_YPLOT_HEADER_SIZE + total_buf_words) * sizeof(uint32_t);
    if (out_capacity < required)
        return YETTY_ERR(yetty_core_size, "buffer too small");

    uint32_t *p = (uint32_t *)out;

    // FAM header
    *p++ = YETTY_YPLOT_TYPE_ID;
    *p++ = (uint32_t)(required - 2 * sizeof(uint32_t));

    // Uniforms
    memcpy(p, uniforms, sizeof(struct yetty_yplot_uniforms));
    p += YETTY_YPLOT_UNIFORMS_SIZE;

    *p++ = (uint32_t)bytecode_len;

    if (bytecode && bytecode_len > 0)
        memcpy(p, bytecode, bytecode_len * sizeof(uint32_t));
    p += bytecode_len;

    return YETTY_OK(yetty_core_size, required);
}
