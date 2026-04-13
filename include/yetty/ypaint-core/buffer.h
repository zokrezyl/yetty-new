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
                             const void *data, size_t size);

// Primitive size callback: given type, return size in bytes (0 = unknown type)
typedef size_t (*yetty_ypaint_core_primitive_size_fn)(uint32_t type);

// Register handler for type range [type_min, type_max] with buffer instance
struct yetty_core_void_result yetty_ypaint_core_buffer_register_handler(
    struct yetty_ypaint_core_buffer *buf,
    uint32_t type_min,
    uint32_t type_max,
    yetty_ypaint_core_primitive_size_fn size_fn);

// Primitive iterator
struct yetty_ypaint_core_primitive_iter {
    const void *data;
    uint32_t type;
    size_t size;
};

YETTY_RESULT_DECLARE(yetty_ypaint_core_primitive_iter, struct yetty_ypaint_core_primitive_iter);

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_first(
    const struct yetty_ypaint_core_buffer *buf);

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_next(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_core_primitive_iter *iter);

#ifdef __cplusplus
}
#endif
