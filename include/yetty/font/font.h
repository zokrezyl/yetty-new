#ifndef YETTY_FONT_FONT_H
#define YETTY_FONT_FONT_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_font_font;
struct yetty_render_gpu_resource_set;

/* Space glyph index - all fonts must return this for U+0020 */
#define YETTY_FONT_SPACE_GLYPH_INDEX 0

/* Font render method - must match shader constants */
enum yetty_font_render_method {
    YETTY_FONT_RENDER_METHOD_MSDF     = 0,
    YETTY_FONT_RENDER_METHOD_BITMAP   = 1,
    YETTY_FONT_RENDER_METHOD_SHADER   = 2,
    YETTY_FONT_RENDER_METHOD_CARD     = 3,
    YETTY_FONT_RENDER_METHOD_VECTOR   = 4,
    YETTY_FONT_RENDER_METHOD_COVERAGE = 5,
    YETTY_FONT_RENDER_METHOD_RASTER   = 6
};

/* Font style */
enum yetty_font_style {
    YETTY_FONT_STYLE_REGULAR     = 0,
    YETTY_FONT_STYLE_BOLD        = 1,
    YETTY_FONT_STYLE_ITALIC      = 2,
    YETTY_FONT_STYLE_BOLD_ITALIC = 3
};

/* Result type */
YETTY_RESULT_DECLARE(yetty_font_font, struct yetty_font_font *);

/* Font ops - polymorphic interface for all font types */
struct yetty_font_font_ops {
    void (*destroy)(struct yetty_font_font *self);

    /* Render method */
    enum yetty_font_render_method (*render_method)(const struct yetty_font_font *self);

    /* Glyph lookup */
    uint32_t (*get_glyph_index)(struct yetty_font_font *self, uint32_t codepoint);
    uint32_t (*get_glyph_index_styled)(struct yetty_font_font *self, uint32_t codepoint,
                                       enum yetty_font_style style);

    /* Cell size */
    void (*set_cell_size)(struct yetty_font_font *self, float cell_width, float cell_height);

    /* Glyph loading */
    struct yetty_core_void_result (*load_glyphs)(struct yetty_font_font *self,
                                                  const uint32_t *codepoints,
                                                  size_t count);
    struct yetty_core_void_result (*load_basic_latin)(struct yetty_font_font *self);

    /* Dirty tracking */
    int (*is_dirty)(const struct yetty_font_font *self);
    void (*clear_dirty)(struct yetty_font_font *self);

    /* GPU resources - provides everything the shader needs */
    struct yetty_render_gpu_resource_set (*get_gpu_resource_set)(const struct yetty_font_font *self);
};

/* Font base */
struct yetty_font_font {
    const struct yetty_font_font_ops *ops;
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_FONT_H */
