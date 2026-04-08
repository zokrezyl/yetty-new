// YPaint Buffer - Implementation

#include <yetty/ypaint/core/ypaint-buffer.h>
#include <stdlib.h>
#include <string.h>

struct YPaintBuffer {
    float* data;
    uint32_t wordCount;
    uint32_t capacity;
};

YPaintBufferHandle ypaint_buffer_create(uint32_t initialCapacity) {
    struct YPaintBuffer* buf = (struct YPaintBuffer*)malloc(sizeof(struct YPaintBuffer));
    if (!buf) {
        return NULL;
    }

    buf->data = (float*)malloc(initialCapacity * sizeof(float));
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->wordCount = 0;
    buf->capacity = initialCapacity;
    return buf;
}

void ypaint_buffer_destroy(YPaintBufferHandle buf) {
    if (buf) {
        free(buf->data);
        free(buf);
    }
}

void ypaint_buffer_clear(YPaintBufferHandle buf) {
    if (buf) {
        buf->wordCount = 0;
    }
}

YPaintIdResult ypaint_buffer_add_prim(YPaintBufferHandle buf, const float* data, uint32_t wordCount) {
    YPaintIdResult result = {0, 0};

    if (!buf || !data) {
        result.error = YPAINT_ERR_NULL;
        return result;
    }

    uint32_t newCount = buf->wordCount + wordCount;

    // Grow if needed
    if (newCount > buf->capacity) {
        uint32_t newCapacity = buf->capacity * 2;
        if (newCapacity < newCount) {
            newCapacity = newCount;
        }

        float* newData = (float*)realloc(buf->data, newCapacity * sizeof(float));
        if (!newData) {
            result.error = YPAINT_ERR_ALLOC;
            return result;
        }

        buf->data = newData;
        buf->capacity = newCapacity;
    }

    // Copy data
    result.id = buf->wordCount;
    memcpy(buf->data + buf->wordCount, data, wordCount * sizeof(float));
    buf->wordCount = newCount;

    return result;
}

uint32_t ypaint_buffer_word_count(YPaintBufferHandle buf) {
    return buf ? buf->wordCount : 0;
}

const float* ypaint_buffer_data(YPaintBufferHandle buf) {
    return buf ? buf->data : NULL;
}

uint32_t ypaint_buffer_capacity(YPaintBufferHandle buf) {
    return buf ? buf->capacity : 0;
}
