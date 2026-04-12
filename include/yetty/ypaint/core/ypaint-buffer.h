// YPaint Buffer - primitive buffer for ypaint
// Pure data container, struct is public for direct field access

#pragma once

#include <yetty/core/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
#define YPAINT_OK           0
#define YPAINT_ERR_NULL     1
#define YPAINT_ERR_OVERFLOW 2
#define YPAINT_ERR_ALLOC    3

// Result from adding a primitive
struct yetty_ypaint_id_result {
    int error;
    uint32_t id;  // byte offset in prims buffer
};

// YPaint buffer - contains multiple named buffers for different data types
struct yetty_ypaint_buffer {
    struct yetty_named_buffer prims;   // raw primitive data (float words)
    // TODO: add when needed
    // struct yetty_text_span *text_spans;
    // uint32_t text_span_count;
    // struct yetty_font_blob *fonts;
    // uint32_t font_count;
    // struct yetty_image_data *images;
    // uint32_t image_count;
};

// Create/destroy
struct yetty_ypaint_buffer *yetty_ypaint_buffer_create(void);
void yetty_ypaint_buffer_destroy(struct yetty_ypaint_buffer *buf);

// Clear all data (keeps allocation)
void yetty_ypaint_buffer_clear(struct yetty_ypaint_buffer *buf);

// Add raw primitive data, returns byte offset
struct yetty_ypaint_id_result yetty_ypaint_buffer_add_prim(
    struct yetty_ypaint_buffer *buf,
    const float *data,
    uint32_t word_count);

#ifdef __cplusplus
}
#endif
