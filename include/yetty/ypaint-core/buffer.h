// YPaint Buffer - primitive buffer for ypaint
// Pure data container, struct is public for direct field access

#pragma once

#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>

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
    const struct yetty_core_buffer *base64_buf);
void yetty_ypaint_core_buffer_destroy(struct yetty_ypaint_core_buffer *buf);

// Clear all data (keeps allocation)
void yetty_ypaint_core_buffer_clear(struct yetty_ypaint_core_buffer *buf);

// Add raw primitive data, returns byte offset
struct yetty_ypaint_id_result
yetty_ypaint_core_buffer_add_prim(struct yetty_ypaint_core_buffer *buf,
                             const float *data, uint32_t word_count);

#ifdef __cplusplus
}
#endif
