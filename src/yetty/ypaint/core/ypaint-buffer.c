// YPaint Buffer - Implementation

#include <yetty/ypaint/core/ypaint-buffer.h>
#include <stdlib.h>
#include <string.h>

struct yetty_ypaint_buffer {
    uint8_t *data;
    uint32_t size;       // current size in bytes
    uint32_t capacity;   // allocated capacity in bytes
};

struct yetty_ypaint_buffer *yetty_ypaint_buffer_create(uint32_t initial_capacity)
{
    struct yetty_ypaint_buffer *buf;

    buf = calloc(1, sizeof(struct yetty_ypaint_buffer));
    if (!buf)
        return NULL;

    buf->data = calloc(1, initial_capacity);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->size = 0;
    buf->capacity = initial_capacity;
    return buf;
}

void yetty_ypaint_buffer_destroy(struct yetty_ypaint_buffer *buf)
{
    if (!buf)
        return;

    free(buf->data);
    free(buf);
}

void yetty_ypaint_buffer_clear(struct yetty_ypaint_buffer *buf)
{
    if (buf)
        buf->size = 0;
}

struct yetty_ypaint_id_result yetty_ypaint_buffer_add_prim(struct yetty_ypaint_buffer *buf,
                                                           const float *data,
                                                           uint32_t word_count)
{
    struct yetty_ypaint_id_result result = {0, 0};

    if (!buf || !data) {
        result.error = YPAINT_ERR_NULL;
        return result;
    }

    uint32_t byte_count = word_count * sizeof(float);
    uint32_t new_size = buf->size + byte_count;

    // Grow if needed
    if (new_size > buf->capacity) {
        uint32_t new_capacity = buf->capacity * 2;
        if (new_capacity < new_size)
            new_capacity = new_size;

        uint8_t *new_data = realloc(buf->data, new_capacity);
        if (!new_data) {
            result.error = YPAINT_ERR_ALLOC;
            return result;
        }

        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    // Copy data
    result.id = buf->size;  // offset in bytes
    memcpy(buf->data + buf->size, data, byte_count);
    buf->size = new_size;

    return result;
}

uint32_t yetty_ypaint_buffer_size(struct yetty_ypaint_buffer *buf)
{
    return buf ? buf->size : 0;
}

const uint8_t *yetty_ypaint_buffer_data(struct yetty_ypaint_buffer *buf)
{
    return buf ? buf->data : NULL;
}

uint32_t yetty_ypaint_buffer_capacity(struct yetty_ypaint_buffer *buf)
{
    return buf ? buf->capacity : 0;
}
