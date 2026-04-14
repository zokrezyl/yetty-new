#ifndef YETTY_CORE_TYPES_H
#define YETTY_CORE_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get pointer to containing struct from member pointer */
#define container_of(ptr, type, member)                                        \
  ((type *)((char *)(ptr) - offsetof(type, member)))

typedef uint64_t yetty_core_object_id;

#define YETTY_CORE_OBJECT_ID_NONE 0

/*=============================================================================
 * Color
 *===========================================================================*/

struct yetty_color_rgba {
  uint8_t r, g, b, a;
};

/*=============================================================================
 * Buffer hierarchy
 *===========================================================================*/

#define YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH 32

struct yetty_core_buffer {
  uint8_t *data;
  size_t capacity;
  size_t size;
};

YETTY_RESULT_DECLARE(yetty_core_buffer, struct yetty_core_buffer);

struct yetty_core_named_buffer {
  struct yetty_core_buffer buf;
  char name[YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH];
};

/*=============================================================================
 * Font blob - named buffer containing TTF data
 *===========================================================================*/

struct yetty_font_blob {
  struct yetty_core_named_buffer named_buf;
  int32_t font_id;
};

/*=============================================================================
 * Image data - named buffer containing RGBA8 pixels
 *===========================================================================*/

struct yetty_image_data {
  struct yetty_core_named_buffer named_buf;
  float x, y, w, h;
  uint32_t pixel_width;
  uint32_t pixel_height;
  uint32_t layer;
  uint32_t atlas_x, atlas_y;
};

/*=============================================================================
 * Text span - named buffer containing UTF-8 text
 *===========================================================================*/

struct yetty_text_span {
  struct yetty_core_named_buffer named_buf;
  float x, y;
  float font_size;
  float rotation;
  struct yetty_color_rgba color;
  uint32_t layer;
  int32_t font_id;
};

struct grid_size {
  uint32_t rows;
  uint32_t cols;
};

struct pixel_size {
  float width;
  float height;
};

struct pixel_coord {
  float x;
  float y;
};

struct grid_cursor_pos {
  uint32_t rows;
  uint32_t cols;
};

struct rectangle {
  struct pixel_coord min;
  struct pixel_coord max;
};

YETTY_RESULT_DECLARE(uint32, uint32_t);
YETTY_RESULT_DECLARE(pixel_size, struct pixel_size);
YETTY_RESULT_DECLARE(rectangle, struct rectangle);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_CORE_TYPES_H */
