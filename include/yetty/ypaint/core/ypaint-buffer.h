// YPaint Buffer - Core primitive buffer for ypaint
// Pure data container with no dependencies

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_ypaint_buffer;

// Result from adding a primitive
struct yetty_ypaint_id_result {
    int error;       // 0 on success, error code otherwise
    uint32_t id;     // primitive ID (offset in buffer)
};

// Error codes
#define YPAINT_OK           0
#define YPAINT_ERR_NULL     1
#define YPAINT_ERR_OVERFLOW 2
#define YPAINT_ERR_ALLOC    3

// Create a new buffer with initial capacity (in bytes)
struct yetty_ypaint_buffer *yetty_ypaint_buffer_create(uint32_t initial_capacity);

// Destroy a buffer
void yetty_ypaint_buffer_destroy(struct yetty_ypaint_buffer *buf);

// Clear all primitives from buffer (keeps allocation)
void yetty_ypaint_buffer_clear(struct yetty_ypaint_buffer *buf);

// Add raw primitive data to buffer
// Returns ID (word offset) of the primitive
struct yetty_ypaint_id_result yetty_ypaint_buffer_add_prim(struct yetty_ypaint_buffer *buf,
                                                           const float *data,
                                                           uint32_t word_count);

// Get current size in bytes
uint32_t yetty_ypaint_buffer_size(struct yetty_ypaint_buffer *buf);

// Get pointer to raw data (for GPU upload)
const uint8_t *yetty_ypaint_buffer_data(struct yetty_ypaint_buffer *buf);

// Get capacity in bytes
uint32_t yetty_ypaint_buffer_capacity(struct yetty_ypaint_buffer *buf);

#ifdef __cplusplus
}
#endif
