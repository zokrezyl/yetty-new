// YPaint Buffer - Implementation

#include <stdlib.h>
#include <string.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/util.h>
#include <yetty/ytrace.h>

#define YPAINT_BUFFER_INITIAL_CAPACITY 1024
#define YPAINT_MAX_HANDLERS 4

// Primitive handler
struct primitive_handler {
    uint32_t type_min;
    uint32_t type_max;
    yetty_ypaint_core_primitive_size_fn size_fn;
};

// YPaint buffer - contains primitive data and handlers
struct yetty_ypaint_core_buffer {
  struct yetty_core_named_buffer primitives;
  struct primitive_handler handlers[YPAINT_MAX_HANDLERS];
  size_t handler_count;
};

struct yetty_core_void_result yetty_ypaint_core_buffer_register_handler(
    struct yetty_ypaint_core_buffer *buf,
    uint32_t type_min,
    uint32_t type_max,
    yetty_ypaint_core_primitive_size_fn size_fn) {
    if (!buf)
        return YETTY_ERR(yetty_core_void, "buf is NULL");
    if (buf->handler_count >= YPAINT_MAX_HANDLERS)
        return YETTY_ERR(yetty_core_void, "max handlers reached");
    if (!size_fn)
        return YETTY_ERR(yetty_core_void, "size_fn is NULL");
    buf->handlers[buf->handler_count].type_min = type_min;
    buf->handlers[buf->handler_count].type_max = type_max;
    buf->handlers[buf->handler_count].size_fn = size_fn;
    buf->handler_count++;
    return YETTY_OK_VOID();
}

static struct yetty_core_size_result get_primitive_size(
    const struct yetty_ypaint_core_buffer *buf, uint32_t type) {
    for (size_t i = 0; i < buf->handler_count; i++) {
        if (type >= buf->handlers[i].type_min && type <= buf->handlers[i].type_max) {
            size_t size = buf->handlers[i].size_fn(type);
            if (size > 0)
                return YETTY_OK(yetty_core_size, size);
        }
    }
    return YETTY_ERR(yetty_core_size, "unknown primitive type");
}

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
yetty_ypaint_core_buffer_add_prim(struct yetty_ypaint_core_buffer *buf,
                                  const void *data, size_t size) {
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

  size_t new_size = buf->primitives.buf.size + size;

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
  memcpy(buf->primitives.buf.data + buf->primitives.buf.size, data, size);
  buf->primitives.buf.size = new_size;

  return result;
}

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_first(
    const struct yetty_ypaint_core_buffer *buf) {
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buf is NULL");
  if (!buf->primitives.buf.data || buf->primitives.buf.size == 0)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buffer empty");

  const uint8_t *data = buf->primitives.buf.data;
  uint32_t type;
  memcpy(&type, data, sizeof(type));

  struct yetty_core_size_result size_res = get_primitive_size(buf, type);
  if (YETTY_IS_ERR(size_res))
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, size_res.error.msg);

  struct yetty_ypaint_core_primitive_iter iter = {
      .data = data,
      .type = type,
      .size = size_res.value,
  };
  return YETTY_OK(yetty_ypaint_core_primitive_iter, iter);
}

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_next(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_core_primitive_iter *iter) {
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buf is NULL");
  if (!iter)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "iter is NULL");

  const uint8_t *base = buf->primitives.buf.data;
  size_t buf_size = buf->primitives.buf.size;
  const uint8_t *next = (const uint8_t *)iter->data + iter->size;
  size_t offset = next - base;

  if (offset >= buf_size)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "end of buffer");

  uint32_t type;
  memcpy(&type, next, sizeof(type));

  struct yetty_core_size_result size_res = get_primitive_size(buf, type);
  if (YETTY_IS_ERR(size_res))
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, size_res.error.msg);

  struct yetty_ypaint_core_primitive_iter new_iter = {
      .data = next,
      .type = type,
      .size = size_res.value,
  };
  return YETTY_OK(yetty_ypaint_core_primitive_iter, new_iter);
}
