// YPaint Buffer - Implementation

#include <stdlib.h>
#include <string.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/util.h>
#include <yetty/ytrace.h>

#define YPAINT_BUFFER_INITIAL_CAPACITY 1024
#define YPAINT_MAX_FONTS 8
#define YPAINT_MAX_TEXT_SPANS 4096

// YPaint buffer - contains primitive data, fonts, text spans
struct yetty_ypaint_core_buffer {
  struct yetty_ycore_named_buffer primitives;

  struct yetty_font_blob fonts[YPAINT_MAX_FONTS];
  uint32_t font_count;

  struct yetty_text_span text_spans[YPAINT_MAX_TEXT_SPANS];
  uint32_t text_span_count;

  float scene_min_x, scene_min_y, scene_max_x, scene_max_y;

  /* serialize() scratch — reused across calls, grows on demand. */
  uint8_t *serial_data;
  size_t serial_cap;
};

/* Framed wire format. Magic chosen so it can't look like a valid
 * ysdf primitive header (primitive types are < 256, scene bounds start
 * with a float). */
#define YPAINT_SERIAL_MAGIC 0x31425059u  /* 'YPB1' little-endian */

/* Read helpers — advance *p if enough bytes remain, else bail. */
static int _read_u32(const uint8_t **p, const uint8_t *end, uint32_t *out) {
  if ((size_t)(end - *p) < 4) return 0;
  memcpy(out, *p, 4); *p += 4; return 1;
}
static int _read_i32(const uint8_t **p, const uint8_t *end, int32_t *out) {
  if ((size_t)(end - *p) < 4) return 0;
  memcpy(out, *p, 4); *p += 4; return 1;
}
static int _read_f32(const uint8_t **p, const uint8_t *end, float *out) {
  if ((size_t)(end - *p) < 4) return 0;
  memcpy(out, *p, 4); *p += 4; return 1;
}

static int parse_framed_payload(struct yetty_ypaint_core_buffer *buf,
                                const uint8_t *data, size_t size) {
  const uint8_t *p = data;
  const uint8_t *end = data + size;

  /* magic already validated by caller; skip past it. */
  p += 4;

  if (!_read_f32(&p, end, &buf->scene_min_x)) return 0;
  if (!_read_f32(&p, end, &buf->scene_min_y)) return 0;
  if (!_read_f32(&p, end, &buf->scene_max_x)) return 0;
  if (!_read_f32(&p, end, &buf->scene_max_y)) return 0;

  uint32_t prim_size;
  if (!_read_u32(&p, end, &prim_size)) return 0;
  if ((size_t)(end - p) < prim_size) return 0;
  if (prim_size > 0) {
    uint8_t *pd = malloc(prim_size);
    if (!pd) return 0;
    memcpy(pd, p, prim_size);
    free(buf->primitives.buf.data);
    buf->primitives.buf.data = pd;
    buf->primitives.buf.size = prim_size;
    buf->primitives.buf.capacity = prim_size;
  }
  p += prim_size;

  uint32_t text_count;
  if (!_read_u32(&p, end, &text_count)) return 0;
  if (text_count > YPAINT_MAX_TEXT_SPANS) text_count = YPAINT_MAX_TEXT_SPANS;
  for (uint32_t i = 0; i < text_count; i++) {
    struct yetty_text_span *ts = &buf->text_spans[i];
    if (!_read_f32(&p, end, &ts->x)) return 0;
    if (!_read_f32(&p, end, &ts->y)) return 0;
    if (!_read_f32(&p, end, &ts->font_size)) return 0;
    if (!_read_f32(&p, end, &ts->rotation)) return 0;
    uint32_t color;
    if (!_read_u32(&p, end, &color)) return 0;
    ts->color.r = color & 0xFF;
    ts->color.g = (color >> 8) & 0xFF;
    ts->color.b = (color >> 16) & 0xFF;
    ts->color.a = (color >> 24) & 0xFF;
    if (!_read_u32(&p, end, &ts->layer)) return 0;
    if (!_read_i32(&p, end, &ts->font_id)) return 0;
    uint32_t tl;
    if (!_read_u32(&p, end, &tl)) return 0;
    if ((size_t)(end - p) < tl) return 0;
    if (tl > 0) {
      ts->named_buf.buf.data = malloc(tl);
      if (!ts->named_buf.buf.data) return 0;
      memcpy(ts->named_buf.buf.data, p, tl);
      ts->named_buf.buf.size = tl;
      ts->named_buf.buf.capacity = tl;
    }
    p += tl;
  }
  buf->text_span_count = text_count;
  return 1;
}

/* Construct from already-decoded bytes. Owns a private copy. */
struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create_from_bytes(
    const uint8_t *data, size_t len) {
  if (!data || len == 0)
    return YETTY_ERR(yetty_ypaint_core_buffer, "null or empty bytes");

  struct yetty_ypaint_core_buffer *buf =
      calloc(1, sizeof(struct yetty_ypaint_core_buffer));
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_buffer, "calloc failed");

  uint8_t *decoded = malloc(len);
  if (!decoded) {
    free(buf);
    return YETTY_ERR(yetty_ypaint_core_buffer, "malloc failed");
  }
  memcpy(decoded, data, len);

  /* Framed (magic-tagged) payload = prims + text_spans + scene_bounds.
   * Otherwise the bytes are a bare primitive stream (legacy path). */
  if (len >= 4 && *(uint32_t *)decoded == YPAINT_SERIAL_MAGIC) {
    if (!parse_framed_payload(buf, decoded, len)) {
      free(decoded);
      yetty_ypaint_core_buffer_destroy(buf);
      return YETTY_ERR(yetty_ypaint_core_buffer, "framed payload parse failed");
    }
    free(decoded);
  } else {
    buf->primitives.buf.data = decoded;
    buf->primitives.buf.capacity = len;
    buf->primitives.buf.size = len;
  }
  strncpy(buf->primitives.name, "prims",
          YETTY_YCORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);
  return YETTY_OK(yetty_ypaint_core_buffer, buf);
}

struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create_from_base64(
    const struct yetty_ycore_buffer *base64_buf) {
  if (!base64_buf || !base64_buf->data || base64_buf->size == 0)
    return YETTY_ERR(yetty_ypaint_core_buffer, "null or empty base64 buffer");

  size_t decoded_cap = (base64_buf->size * 3) / 4 + 4;
  uint8_t *decoded = malloc(decoded_cap);
  if (!decoded)
    return YETTY_ERR(yetty_ypaint_core_buffer, "malloc failed");
  size_t decoded_len = yetty_ycore_base64_decode(
      (const char *)base64_buf->data, base64_buf->size,
      (char *)decoded, decoded_cap);

  struct yetty_ypaint_core_buffer_result r =
      yetty_ypaint_core_buffer_create_from_bytes(decoded, decoded_len);
  free(decoded);
  return r;
}

struct yetty_ypaint_core_buffer_result yetty_ypaint_core_buffer_create(
    const struct yetty_ypaint_core_buffer_config *config) {
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
          YETTY_YCORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);

  if (config) {
    buf->scene_min_x = config->scene_min_x;
    buf->scene_min_y = config->scene_min_y;
    buf->scene_max_x = config->scene_max_x;
    buf->scene_max_y = config->scene_max_y;
  }

  return YETTY_OK(yetty_ypaint_core_buffer, buf);
}

float yetty_ypaint_core_buffer_scene_min_x(const struct yetty_ypaint_core_buffer *buf) {
  return buf ? buf->scene_min_x : 0.0f;
}
float yetty_ypaint_core_buffer_scene_min_y(const struct yetty_ypaint_core_buffer *buf) {
  return buf ? buf->scene_min_y : 0.0f;
}
float yetty_ypaint_core_buffer_scene_max_x(const struct yetty_ypaint_core_buffer *buf) {
  return buf ? buf->scene_max_x : 0.0f;
}
float yetty_ypaint_core_buffer_scene_max_y(const struct yetty_ypaint_core_buffer *buf) {
  return buf ? buf->scene_max_y : 0.0f;
}

void yetty_ypaint_core_buffer_destroy(struct yetty_ypaint_core_buffer *buf) {
  if (!buf)
    return;

  free(buf->primitives.buf.data);
  free(buf->serial_data);

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
  /* Free any text-span data allocated between clears, then reset count. */
  for (uint32_t i = 0; i < buf->text_span_count; i++) {
    free(buf->text_spans[i].named_buf.buf.data);
    buf->text_spans[i].named_buf.buf.data = NULL;
    buf->text_spans[i].named_buf.buf.size = 0;
  }
  buf->text_span_count = 0;
}

const void *yetty_ypaint_core_buffer_data(const struct yetty_ypaint_core_buffer *buf) {
  return buf ? buf->primitives.buf.data : NULL;
}

size_t yetty_ypaint_core_buffer_size(const struct yetty_ypaint_core_buffer *buf) {
  return buf ? buf->primitives.buf.size : 0;
}

void yetty_ypaint_core_buffer_set_scene_bounds(struct yetty_ypaint_core_buffer *buf,
                                               float min_x, float min_y,
                                               float max_x, float max_y) {
  if (!buf)
    return;
  buf->scene_min_x = min_x;
  buf->scene_min_y = min_y;
  buf->scene_max_x = max_x;
  buf->scene_max_y = max_y;
}

/* Shared emitter for the framed wire format. Both _serialize (writes raw
 * bytes into the reusable scratch) and _to_base64 (streams base64 chars
 * directly into a caller-sized output, no intermediate blob) feed their
 * sinks through this. Mirror of parse_framed_payload — layouts must stay
 * in sync. */
typedef void (*ypaint_emit_fn)(const void *data, size_t size, void *ctx);

static size_t ypaint_framed_size(const struct yetty_ypaint_core_buffer *buf) {
  size_t need = 4 + 16 + 4 + buf->primitives.buf.size + 4;
  for (uint32_t i = 0; i < buf->text_span_count; i++) {
    /* x,y,font_size,rotation,color,layer,font_id,text_len + payload */
    need += 4 * 7 + 4 + buf->text_spans[i].named_buf.buf.size;
  }
  return need;
}

static void ypaint_framed_emit(const struct yetty_ypaint_core_buffer *buf,
                                ypaint_emit_fn emit, void *ctx) {
  uint32_t u;
  u = YPAINT_SERIAL_MAGIC;           emit(&u, 4, ctx);
  emit(&buf->scene_min_x, 4, ctx);
  emit(&buf->scene_min_y, 4, ctx);
  emit(&buf->scene_max_x, 4, ctx);
  emit(&buf->scene_max_y, 4, ctx);
  u = (uint32_t)buf->primitives.buf.size;
  emit(&u, 4, ctx);
  if (buf->primitives.buf.size > 0)
    emit(buf->primitives.buf.data, buf->primitives.buf.size, ctx);
  u = buf->text_span_count;
  emit(&u, 4, ctx);
  for (uint32_t i = 0; i < buf->text_span_count; i++) {
    const struct yetty_text_span *ts = &buf->text_spans[i];
    emit(&ts->x, 4, ctx);
    emit(&ts->y, 4, ctx);
    emit(&ts->font_size, 4, ctx);
    emit(&ts->rotation, 4, ctx);
    uint32_t color = (uint32_t)ts->color.r
                   | ((uint32_t)ts->color.g << 8)
                   | ((uint32_t)ts->color.b << 16)
                   | ((uint32_t)ts->color.a << 24);
    emit(&color, 4, ctx);
    emit(&ts->layer, 4, ctx);
    emit(&ts->font_id, 4, ctx);
    uint32_t tl = (uint32_t)ts->named_buf.buf.size;
    emit(&tl, 4, ctx);
    if (tl > 0)
      emit(ts->named_buf.buf.data, tl, ctx);
  }
}

/* Sink 1: raw bytes, advancing a write cursor. */
struct ypaint_raw_sink { uint8_t *p; };
static void ypaint_raw_emit(const void *data, size_t size, void *ctx) {
  struct ypaint_raw_sink *s = ctx;
  memcpy(s->p, data, size);
  s->p += size;
}

/* Sink 2: streaming base64 — 3-byte rolling window, 4 chars at a time. */
static const char YPAINT_B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct ypaint_b64_sink {
  char   *out;
  size_t  olen;
  uint8_t window[3];
  int     wc;        /* 0..3 bytes currently buffered in `window` */
};

static inline void ypaint_b64_flush_triple(struct ypaint_b64_sink *s) {
  uint32_t t = ((uint32_t)s->window[0] << 16)
             | ((uint32_t)s->window[1] <<  8)
             |  (uint32_t)s->window[2];
  s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >> 18) & 0x3F];
  s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >> 12) & 0x3F];
  s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >>  6) & 0x3F];
  s->out[s->olen++] = YPAINT_B64_ALPHABET[ t        & 0x3F];
  s->wc = 0;
}

static void ypaint_b64_emit(const void *data, size_t size, void *ctx) {
  struct ypaint_b64_sink *s = ctx;
  const uint8_t *p = data;
  while (size > 0) {
    while (s->wc < 3 && size > 0) {
      s->window[s->wc++] = *p++;
      size--;
    }
    if (s->wc == 3)
      ypaint_b64_flush_triple(s);
  }
}

static void ypaint_b64_finalize(struct ypaint_b64_sink *s) {
  if (s->wc == 1) {
    uint32_t t = (uint32_t)s->window[0] << 16;
    s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >> 18) & 0x3F];
    s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >> 12) & 0x3F];
    s->out[s->olen++] = '=';
    s->out[s->olen++] = '=';
  } else if (s->wc == 2) {
    uint32_t t = ((uint32_t)s->window[0] << 16)
               | ((uint32_t)s->window[1] <<  8);
    s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >> 18) & 0x3F];
    s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >> 12) & 0x3F];
    s->out[s->olen++] = YPAINT_B64_ALPHABET[(t >>  6) & 0x3F];
    s->out[s->olen++] = '=';
  }
  s->out[s->olen] = '\0';
}

size_t yetty_ypaint_core_buffer_serialize(
    struct yetty_ypaint_core_buffer *buf, const uint8_t **out_data) {
  if (!buf || !out_data) {
    if (out_data) *out_data = NULL;
    return 0;
  }

  size_t need = ypaint_framed_size(buf);

  if (buf->serial_cap < need) {
    uint8_t *np = realloc(buf->serial_data, need);
    if (!np) {
      *out_data = NULL;
      return 0;
    }
    buf->serial_data = np;
    buf->serial_cap = need;
  }

  struct ypaint_raw_sink sink = { .p = buf->serial_data };
  ypaint_framed_emit(buf, ypaint_raw_emit, &sink);

  *out_data = buf->serial_data;
  return need;
}

/* Single-pass, single-allocation base64 of the framed wire format. Writes
 * base64 chars directly into the output as the framed fields are produced
 * — no intermediate raw blob, no double copy. */
struct yetty_ycore_buffer_result yetty_ypaint_core_buffer_to_base64(
    const struct yetty_ypaint_core_buffer *buf) {
  if (!buf)
    return YETTY_ERR(yetty_ycore_buffer, "buf is NULL");

  size_t need = ypaint_framed_size(buf);
  size_t cap = ((need + 2) / 3) * 4 + 1;  /* base64 len + NUL */
  char *out = malloc(cap);
  if (!out)
    return YETTY_ERR(yetty_ycore_buffer, "malloc failed");

  struct ypaint_b64_sink sink = { .out = out, .olen = 0, .wc = 0 };
  ypaint_framed_emit(buf, ypaint_b64_emit, &sink);
  ypaint_b64_finalize(&sink);

  struct yetty_ycore_buffer b = {0};
  b.data = (uint8_t *)out;
  b.size = sink.olen;
  b.capacity = cap;
  return YETTY_OK(yetty_ycore_buffer, b);
}

const struct yetty_ycore_buffer *
yetty_ypaint_core_buffer_primitives(const struct yetty_ypaint_core_buffer *buf) {
  if (!buf)
    return NULL;
  return &buf->primitives.buf;
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
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_flyweight_registry *reg) {
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buf is NULL");
  if (!reg)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "reg is NULL");
  if (!buf->primitives.buf.data || buf->primitives.buf.size == 0)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buffer empty");

  const uint32_t *prim = (const uint32_t *)buf->primitives.buf.data;
  uint32_t prim_type = prim[0];
  struct yetty_ypaint_prim_flyweight_ptr_result fw_res =
      yetty_ypaint_flyweight_registry_get(reg, prim_type, prim);
  if (YETTY_IS_ERR(fw_res))
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, fw_res.error.msg);

  struct yetty_ypaint_core_primitive_iter iter = {.fw = *fw_res.value};
  return YETTY_OK(yetty_ypaint_core_primitive_iter, iter);
}

struct yetty_ypaint_core_primitive_iter_result yetty_ypaint_core_buffer_prim_next(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_flyweight_registry *reg,
    const struct yetty_ypaint_core_primitive_iter *iter) {
  if (!buf)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "buf is NULL");
  if (!reg)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "reg is NULL");
  if (!iter || !iter->fw.ops)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "iter is NULL");

  const uint8_t *base = buf->primitives.buf.data;
  size_t buf_size = buf->primitives.buf.size;
  struct yetty_ycore_size_result size_res = iter->fw.ops->size(iter->fw.data);
  if (YETTY_IS_ERR(size_res))
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, size_res.error.msg);
  const uint32_t *next = (const uint32_t *)((const uint8_t *)iter->fw.data + size_res.value);
  size_t offset = (const uint8_t *)next - base;

  if (offset >= buf_size)
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, "end of buffer");

  uint32_t prim_type = next[0];
  struct yetty_ypaint_prim_flyweight_ptr_result fw_res =
      yetty_ypaint_flyweight_registry_get(reg, prim_type, next);
  if (YETTY_IS_ERR(fw_res))
    return YETTY_ERR(yetty_ypaint_core_primitive_iter, fw_res.error.msg);

  struct yetty_ypaint_core_primitive_iter new_iter = {.fw = *fw_res.value};
  return YETTY_OK(yetty_ypaint_core_primitive_iter, new_iter);
}

/*=============================================================================
 * Font blob storage
 *===========================================================================*/

struct yetty_ycore_int_result
yetty_ypaint_core_buffer_add_font(struct yetty_ypaint_core_buffer *buf,
                                  const struct yetty_ycore_buffer *ttf_data,
                                  const char *name)
{
  if (!buf)
    return YETTY_ERR(yetty_ycore_int, "buf is NULL");
  if (!ttf_data || !ttf_data->data || ttf_data->size == 0)
    return YETTY_ERR(yetty_ycore_int, "ttf_data is empty");
  if (buf->font_count >= YPAINT_MAX_FONTS)
    return YETTY_ERR(yetty_ycore_int, "max fonts reached");

  struct yetty_font_blob *fb = &buf->fonts[buf->font_count];
  fb->font_id = (int32_t)buf->font_count;

  /* Copy TTF data */
  fb->named_buf.buf.data = malloc(ttf_data->size);
  if (!fb->named_buf.buf.data)
    return YETTY_ERR(yetty_ycore_int, "allocation failed");
  memcpy(fb->named_buf.buf.data, ttf_data->data, ttf_data->size);
  fb->named_buf.buf.size = ttf_data->size;
  fb->named_buf.buf.capacity = ttf_data->size;

  if (name) {
    strncpy(fb->named_buf.name, name,
            YETTY_YCORE_NAMED_BUFFER_MAX_NAME_LENGTH - 1);
  }

  int id = (int)buf->font_count;
  buf->font_count++;
  return YETTY_OK(yetty_ycore_int, id);
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

struct yetty_ycore_void_result
yetty_ypaint_core_buffer_add_text(struct yetty_ypaint_core_buffer *buf,
                                  float x, float y,
                                  const struct yetty_ycore_buffer *text,
                                  float font_size, uint32_t color,
                                  uint32_t layer, int32_t font_id,
                                  float rotation)
{
  if (!buf)
    return YETTY_ERR(yetty_ycore_void, "buf is NULL");
  if (!text || !text->data || text->size == 0)
    return YETTY_ERR(yetty_ycore_void, "text is empty");
  if (buf->text_span_count >= YPAINT_MAX_TEXT_SPANS)
    return YETTY_ERR(yetty_ycore_void, "max text spans reached");

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
    return YETTY_ERR(yetty_ycore_void, "allocation failed");
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
