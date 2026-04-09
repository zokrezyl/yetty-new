// YPaint Buffer - Core primitive buffer for ypaint
// Pure data container with no dependencies

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a YPaintBuffer
typedef struct YPaintBuffer* YPaintBufferHandle;

// Result from adding a primitive
typedef struct YPaintIdResult {
    int error;       // 0 on success, error code otherwise
    uint32_t id;     // primitive ID (offset in buffer)
} YPaintIdResult;

// Error codes
#define YPAINT_OK           0
#define YPAINT_ERR_NULL     1
#define YPAINT_ERR_OVERFLOW 2
#define YPAINT_ERR_ALLOC    3

// Create a new buffer with initial capacity (in words)
YPaintBufferHandle ypaint_buffer_create(uint32_t initialCapacity);

// Destroy a buffer
void ypaint_buffer_destroy(YPaintBufferHandle buf);

// Clear all primitives from buffer (keeps allocation)
void ypaint_buffer_clear(YPaintBufferHandle buf);

// Add raw primitive data to buffer
// Returns ID (word offset) of the primitive
YPaintIdResult ypaint_buffer_add_prim(YPaintBufferHandle buf, const float* data, uint32_t wordCount);

// Get current word count in buffer
uint32_t ypaint_buffer_word_count(YPaintBufferHandle buf);

// Get pointer to raw data (for GPU upload)
const float* ypaint_buffer_data(YPaintBufferHandle buf);

// Get capacity in words
uint32_t ypaint_buffer_capacity(YPaintBufferHandle buf);

#ifdef __cplusplus
}
#endif
