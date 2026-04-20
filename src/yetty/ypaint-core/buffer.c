// YPaint Buffer - Implementation

#include <stdlib.h>
#include <string.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/util.h>
#include <yetty/ytrace.h>

#define YPAINT_BUFFER_INITIAL_CAPACITY 1024
#define YPAINT_MAX_HANDLERS 4

// Handler registration
struct primitive_handler_reg {
    uint32_t type_min;
    uint32_t type_max;
    yetty_ypaint_prim_handler_fn handler;
};

#define YPAINT_MAX_FONTS 8
#define YPAINT_MAX_TEXT_SPANS 64

// YPaint buffer - contains primitive data, fonts, text spans
struct yetty_ypaint_core_buffer {
  struct yetty_core_named_buffer primitives;
  yetty_ypaint_prim_handler_fn default_handler;
  struct primitive_handler_reg handlers[YPAINT_MAX_HANDLERS];
  size_t handler_count;

  struct yetty_font_blob fonts[YPAINT_MAX_FONTS];
  uint32_t font_count;

  struct yetty_text_span text_spans[YPAINT_MAX_TEXT_SPANS];
  uint32_t text_span_count;
};

void yetty_ypaint_core_buffer_set_default_handler(
    struct yetty_ypaint_core_buffer *buf,
    yetty_ypaint_prim_handler_fn handler) {
    if (buf)
        buf->default_handler = handler;
}

struct yetty_core_void_result yetty_ypaint_core_buffer_register_handler(
    struct yetty_ypaint_core_buffer *buf,
    uint32_t type_min,
    uint32_t type_max,
    yetty_ypaint_prim_handler_fn handler) {
    if (!buf)
        return YETTY_ERR(yetty_core_void, "buf is NULL");
    if (buf->handler_count >= YPAINT_MAX_HANDLERS)
        return YETTY_ERR(yetty_core_void, "max handlers reached");
    if (!handler)
        return YETTY_ERR(yetty_core_void, "handler is NULL");
    buf->handlers[buf->handler_count].type_min = type_min;
    buf->handlers[buf->handler_count].type_max = type_max;
    buf->handlers[buf->handler_count].handler = handler;
    buf->handler_count++;
    return YETTY_OK_VOID();
}

// Get flyweight for primitive at position
static struct yetty_ypaint_prim_flyweight get_flyweight(
    const struct yetty_ypaint_core_buffer *buf, const uint32_t *prim) {
    struct yetty_ypaint_prim_flyweight fw = {NULL, NULL};

    // Try default handler first (fast path for SDF types)
    if (buf->default_handler) {
        fw = buf->default_handler(prim);
        if (fw.ops)
            return fw;
    }
    // Fallback to additional handlers by type range
    uint32_t type = prim[0];
    for (size_t i = 0; i < buf->handler_count; i++) {
        if (type >= buf->handlers[i].type_min && type <= buf->handlers[i].type_max) {
            fw = buf->handlers[i].handler(prim);
            if (fw.ops)
                return fw;
        }
    }
    return fw;  // ops=NULL means unhandled
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

  /* Free text span data (text_spans is embedded array, only free inner data) */
  for (uint32_t i = 0; i < buf->text_span_count; i++)
    free(buf->text_spans[i].named_buf.buf.data);

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

  const uint32_t *prim = (const uint32_t *)buf->primitives.buf.data;
  struct yetty_ypaint_prim_flyweight fw = get_flyweight(buf, prim);
  if (!fw.ops)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "unknown primitive type");

  struct yetty_ypaint_core_primitive_iter iter = {.fw = fw};
  return YETTY_OK(yetty_ypaint_core_primitive_iter, iter);
}

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_next(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_core_primitive_iter *iter) {
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buf is NULL");
  if (!iter || !iter->fw.ops)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "iter is NULL");

  const uint8_t *base = buf->primitives.buf.data;
  size_t buf_size = buf->primitives.buf.size;
  struct yetty_core_size_result size_res = iter->fw.ops->size(iter->fw.data);
  if (YETTY_IS_ERR(size_res))
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, size_res.error.msg);
  const uint32_t *next = (const uint32_t *)((const uint8_t *)iter->fw.data + size_res.value);
  size_t offset = (const uint8_t *)next - base;

  if (offset >= buf_size)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "end of buffer");

  struct yetty_ypaint_prim_flyweight fw = get_flyweight(buf, next);
  if (!fw.ops)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "unknown primitive type");

  struct yetty_ypaint_core_primitive_iter new_iter = {.fw = fw};
  return YETTY_OK(yetty_ypaint_core_primitive_iter, new_iter);
}

/*=============================================================================
 * Font blob storage
 *===========================================================================*/

struct yetty_core_int_result
yetty_ypaint_core_buffer_add_font(struct yetty_ypaint_core_buffer *buf,
                                  const struct yetty_core_buffer *ttf_data,
                                  const char *name)
{
  if (!buf)
    return YETTY_ERR(yetty_core_int, "buf is NULL");
  if (!ttf_data || !ttf_data->data || ttf_data->size == 0)
    return YETTY_ERR(yetty_core_int, "ttf_data is empty");
  if (buf->font_count >= YPAINT_MAX_FONTS)
    return YETTY_ERR(yetty_core_int, "max fonts reached");

  struct yetty_font_blob *fb = &buf->fonts[buf->font_count];
  fb->font_id = (int32_t)buf->font_count;

  /* Copy TTF data */
  fb->named_buf.buf.data = malloc(ttf_data->size);
  if (!fb->named_buf.buf.data)
    return YETTY_ERR(yetty_core_int, "allocation failed");
  memcpy(fb->named_buf.buf.data, ttf_data->data, ttf_data->size);
  fb->named_buf.buf.size = ttf_data->size;
  fb->named_buf.buf.capacity = ttf_data->size;

  if (name) {
    strncpy(fb->named_buf.name, name,
            YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);
  }

  int id = (int)buf->font_count;
  buf->font_count++;
  return YETTY_OK(yetty_core_int, id);
}

uint32_t yetty_ypaint_core_buffer_font_count(
    const struct yetty_ypaint_core_buffer *buf)
{
  return buf ? buf->font_count : 0;
}

const struct yetty_font_blob *yetty_ypaint_core_buffer_get_font(
    const struct yetty_ypaint_core_buffer *buf, uint32_t index)
{
  if (!buf || index >= buf->font_count)
    return NULL;
  return &buf->fonts[index];
}

/*=============================================================================
 * Text span storage
 *===========================================================================*/

struct yetty_core_void_result
yetty_ypaint_core_buffer_add_text(struct yetty_ypaint_core_buffer *buf,
                                  float x, float y,
                                  const struct yetty_core_buffer *text,
                                  float font_size, uint32_t color,
                                  uint32_t layer, int32_t font_id,
                                  float rotation)
{
  if (!buf)
    return YETTY_ERR(yetty_core_void, "buf is NULL");
  if (!text || !text->data || text->size == 0)
    return YETTY_ERR(yetty_core_void, "text is empty");
  if (buf->text_span_count >= YPAINT_MAX_TEXT_SPANS)
    return YETTY_ERR(yetty_core_void, "max text spans reached");

  struct yetty_text_span *ts = &buf->text_spans[buf->text_span_count];
  ts->x = x;
  ts->y = y;
  ts->font_size = font_size;
  ts->color.r = (uint8_t)(color & 0xFF);
  ts->color.g = (uint8_t)((color >> 8) & 0xFF);
  ts->color.b = (uint8_t)((color >> 16) & 0xFF);
  ts->color.a = (uint8_t)((color >> 24) & 0xFF);
  ts->layer = layer;
  ts->font_id = font_id;
  ts->rotation = rotation;

  /* Copy text data */
  ts->named_buf.buf.data = malloc(text->size);
  if (!ts->named_buf.buf.data)
    return YETTY_ERR(yetty_core_void, "allocation failed");
  memcpy(ts->named_buf.buf.data, text->data, text->size);
  ts->named_buf.buf.size = text->size;
  ts->named_buf.buf.capacity = text->size;

  buf->text_span_count++;
  return YETTY_OK_VOID();
}

uint32_t yetty_ypaint_core_buffer_text_span_count(
    const struct yetty_ypaint_core_buffer *buf)
{
  return buf ? buf->text_span_count : 0;
}

const struct yetty_text_span *yetty_ypaint_core_buffer_get_text_span(
    const struct yetty_ypaint_core_buffer *buf, uint32_t index)
{
  if (!buf || index >= buf->text_span_count)
    return NULL;
  return &buf->text_spans[index];
}
