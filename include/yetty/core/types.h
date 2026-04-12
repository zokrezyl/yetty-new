#ifndef YETTY_CORE_TYPES_H
#define YETTY_CORE_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get pointer to containing struct from member pointer */
#define container_of(ptr, type, member) \
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

struct yetty_buffer {
    uint8_t *data;
    size_t capacity;
    size_t size;
};

struct yetty_named_buffer {
    struct yetty_buffer buf;
    char name[YETTY_CORE_NAMED_BUFFER_MAX_NAME_LENGTH];
};

/*=============================================================================
 * Font blob - named buffer containing TTF data
 *===========================================================================*/

struct yetty_font_blob {
    struct yetty_named_buffer named_buf;
    int32_t font_id;
};

/*=============================================================================
 * Image data - named buffer containing RGBA8 pixels
 *===========================================================================*/

struct yetty_image_data {
    struct yetty_named_buffer named_buf;
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
    struct yetty_named_buffer named_buf;
    float x, y;
    float font_size;
    float rotation;
    struct yetty_color_rgba color;
    uint32_t layer;
    int32_t font_id;
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_CORE_TYPES_H */
