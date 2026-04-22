// YPaint Buffer - primitive buffer for ypaint
// Pure data container, struct is public for direct field access

#pragma once

#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/ypaint-core/flyweight.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
#define YPAINT_OK 0
#define YPAINT_ERR_NULL 1
#define YPAINT_ERR_OVERFLOW 2
#define YPAINT_ERR_ALLOC 3

// Result from adding a primitive
struct yetty_ypaint_id_result {
  int error;
  uint32_t id; // byte offset in prims buffer
};

struct yetty_ypaint_core_buffer;

YETTY_RESULT_DECLARE(yetty_ypaint_core_buffer, struct yetty_ypaint_core_buffer *);

// Create/destroy
struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create(void);
struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create_from_base64(
    const struct yetty_ycore_buffer *base64_buf);
void yetty_ypaint_core_buffer_destroy(struct yetty_ypaint_core_buffer *buf);

// Clear all data (keeps allocation)
void yetty_ypaint_core_buffer_clear(struct yetty_ypaint_core_buffer *buf);

// Add raw primitive data, returns byte offset
struct yetty_ypaint_id_result
yetty_ypaint_core_buffer_add_prim(struct yetty_ypaint_core_buffer *buf,
                             const void *data, size_t size);

// Primitive iterator
struct yetty_ypaint_core_primitive_iter {
    struct yetty_ypaint_prim_flyweight fw;
};

YETTY_RESULT_DECLARE(yetty_ypaint_core_primitive_iter, struct yetty_ypaint_core_primitive_iter);

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_first(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_flyweight_registry *reg);

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_next(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_flyweight_registry *reg,
    const struct yetty_ypaint_core_primitive_iter *iter);

/*=============================================================================
 * Font blob storage
 *===========================================================================*/

/* Add font TTF data to buffer. Returns fontId for use in text spans. */
struct yetty_ycore_int_result
yetty_ypaint_core_buffer_add_font(struct yetty_ypaint_core_buffer *buf,
                                  const struct yetty_ycore_buffer *ttf_data,
                                  const char *name);

uint32_t yetty_ypaint_core_buffer_font_count(
    const struct yetty_ypaint_core_buffer *buf);

const struct yetty_font_blob *yetty_ypaint_core_buffer_get_font(
    const struct yetty_ypaint_core_buffer *buf, uint32_t index);

/*=============================================================================
 * Text span storage
 *===========================================================================*/

struct yetty_ycore_void_result
yetty_ypaint_core_buffer_add_text(struct yetty_ypaint_core_buffer *buf,
                                  float x, float y,
                                  const struct yetty_ycore_buffer *text,
                                  float font_size, uint32_t color,
                                  uint32_t layer, int32_t font_id,
                                  float rotation);

uint32_t yetty_ypaint_core_buffer_text_span_count(
    const struct yetty_ypaint_core_buffer *buf);

const struct yetty_text_span *yetty_ypaint_core_buffer_get_text_span(
    const struct yetty_ypaint_core_buffer *buf, uint32_t index);

#ifdef __cplusplus
}
#endif
