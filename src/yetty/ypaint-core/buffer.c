// YPaint Buffer - Implementation

#include <stdlib.h>
#include <string.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/util.h>
#include <yetty/ytrace.h>

#define YPAINT_BUFFER_INITIAL_CAPACITY 1024

// YPaint buffer - contains multiple named buffers for different data types
struct yetty_ypaint_core_buffer {
  struct yetty_core_named_buffer primitives; // raw primitive data (float words)
                                             // TODO: add when needed
  // struct yetty_text_span *text_spans;
  // uint32_t text_span_count;
  // struct yetty_font_blob *fonts;
  // uint32_t font_count;
  // struct yetty_image_data *images;
  // uint32_t image_count;
};

struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create_from_base64(
    const struct yetty_core_buffer *base64_buf) {
  if (!base64_buf || !base64_buf->data || base64_buf->size == 0)
    return YETTY_ERR(yetty_ypaint_core_buffer, "null or empty base64 buffer");

  // Decoded size is at most 3/4 of input
  size_t decoded_cap = (base64_buf->size * 3) / 4 + 4;

  struct yetty_ypaint_core_buffer *buf =
      calloc(1, sizeof(struct yetty_ypaint_core_buffer));
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_buffer, "calloc failed");

  buf->primitives.buf.data = calloc(1, decoded_cap);
  if (!buf->primitives.buf.data) {
    free(buf);
    return YETTY_ERR(yetty_ypaint_core_buffer, "calloc for data failed");
  }
  buf->primitives.buf.capacity = decoded_cap;
  strncpy(buf->primitives.name, "prims", YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);

  // Decode using existing util function
  buf->primitives.buf.size = yetty_core_base64_decode(
      (const char *)base64_buf->data, base64_buf->size,
      (char *)buf->primitives.buf.data, decoded_cap);

  return YETTY_OK(yetty_ypaint_core_buffer, buf);
}

struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create(void) {
  struct yetty_ypaint_core_buffer *buf =
      calloc(1, sizeof(struct yetty_ypaint_core_buffer));
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_buffer, "calloc failed");

  buf->primitives.buf.data = calloc(1, YPAINT_BUFFER_INITIAL_CAPACITY);
  if (!buf->primitives.buf.data) {
    free(buf);
    return YETTY_ERR(yetty_ypaint_core_buffer, "calloc for prims failed");
  }

  buf->primitives.buf.capacity = YPAINT_BUFFER_INITIAL_CAPACITY;
  buf->primitives.buf.size = 0;
  strncpy(buf->primitives.name, "prims",
          YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);

  return YETTY_OK(yetty_ypaint_core_buffer, buf);
}

void yetty_ypaint_core_buffer_destroy(struct yetty_ypaint_core_buffer *buf) {
  if (!buf)
    return;

  free(buf->primitives.buf.data);
  free(buf);
}

void yetty_ypaint_core_buffer_clear(struct yetty_ypaint_core_buffer *buf) {
  if (!buf) {
    yerror("yetty_ypaint_core_buffer_clear: buf is NULL");
    return;
  }
  buf->primitives.buf.size = 0;
}

struct yetty_ypaint_id_result
yetty_ypaint_core_buffer_add_prim(struct yetty_ypaint_core_buffer *buf, const float *data,
                             uint32_t word_count) {
  struct yetty_ypaint_id_result result = {0, 0};

  if (!buf) {
    yerror("yetty_ypaint_core_buffer_add_prim: buf is NULL");
    result.error = YPAINT_ERR_NULL;
    return result;
  }
  if (!data) {
    yerror("yetty_ypaint_core_buffer_add_prim: data is NULL");
    result.error = YPAINT_ERR_NULL;
    return result;
  }

  uint32_t byte_count = word_count * sizeof(float);
  size_t new_size = buf->primitives.buf.size + byte_count;

  // Grow if needed
  if (new_size > buf->primitives.buf.capacity) {
    size_t new_capacity = buf->primitives.buf.capacity * 2;
    if (new_capacity < new_size)
      new_capacity = new_size;

    uint8_t *new_data = realloc(buf->primitives.buf.data, new_capacity);
    if (!new_data) {
      yerror("yetty_ypaint_core_buffer_add_prim: realloc failed for %zu bytes",
             new_capacity);
      result.error = YPAINT_ERR_ALLOC;
      return result;
    }

    buf->primitives.buf.data = new_data;
    buf->primitives.buf.capacity = new_capacity;
  }

  result.id = (uint32_t)buf->primitives.buf.size;
  memcpy(buf->primitives.buf.data + buf->primitives.buf.size, data, byte_count);
  buf->primitives.buf.size = new_size;

  return result;
}
