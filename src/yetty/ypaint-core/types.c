// YPaint Buffer - Implementation

#include <yetty/ypaint-core/buffer.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define YPAINT_BUFFER_INITIAL_CAPACITY 1024

struct yetty_ypaint_buffer *yetty_ypaint_buffer_create(void)
{
    struct yetty_ypaint_buffer *buf = calloc(1, sizeof(struct yetty_ypaint_buffer));
    if (!buf) {
        yerror("yetty_ypaint_buffer_create: calloc failed");
        return NULL;
    }

    buf->prims.buf.data = calloc(1, YPAINT_BUFFER_INITIAL_CAPACITY);
    if (!buf->prims.buf.data) {
        yerror("yetty_ypaint_buffer_create: calloc for prims failed");
        free(buf);
        return NULL;
    }

    buf->prims.buf.capacity = YPAINT_BUFFER_INITIAL_CAPACITY;
    buf->prims.buf.size = 0;
    strncpy(buf->prims.name, "prims", YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);

    return buf;
}

void yetty_ypaint_buffer_destroy(struct yetty_ypaint_buffer *buf)
{
    if (!buf)
        return;

    free(buf->prims.buf.data);
    free(buf);
}

void yetty_ypaint_buffer_clear(struct yetty_ypaint_buffer *buf)
{
    if (!buf) {
        yerror("yetty_ypaint_buffer_clear: buf is NULL");
        return;
    }
    buf->prims.buf.size = 0;
}

struct yetty_ypaint_id_result yetty_ypaint_buffer_add_prim(
    struct yetty_ypaint_buffer *buf,
    const float *data,
    uint32_t word_count)
{
    struct yetty_ypaint_id_result result = {0, 0};

    if (!buf) {
        yerror("yetty_ypaint_buffer_add_prim: buf is NULL");
        result.error = YPAINT_ERR_NULL;
        return result;
    }
    if (!data) {
        yerror("yetty_ypaint_buffer_add_prim: data is NULL");
        result.error = YPAINT_ERR_NULL;
        return result;
    }

    uint32_t byte_count = word_count * sizeof(float);
    size_t new_size = buf->prims.buf.size + byte_count;

    // Grow if needed
    if (new_size > buf->prims.buf.capacity) {
        size_t new_capacity = buf->prims.buf.capacity * 2;
        if (new_capacity < new_size)
            new_capacity = new_size;

        uint8_t *new_data = realloc(buf->prims.buf.data, new_capacity);
        if (!new_data) {
            yerror("yetty_ypaint_buffer_add_prim: realloc failed for %zu bytes", new_capacity);
            result.error = YPAINT_ERR_ALLOC;
            return result;
        }

        buf->prims.buf.data = new_data;
        buf->prims.buf.capacity = new_capacity;
    }

    result.id = (uint32_t)buf->prims.buf.size;
    memcpy(buf->prims.buf.data + buf->prims.buf.size, data, byte_count);
    buf->prims.buf.size = new_size;

    return result;
}
