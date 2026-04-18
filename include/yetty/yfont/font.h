#ifndef YETTY_FONT_FONT_H
#define YETTY_FONT_FONT_H

/*
 * yetty_font_font - Non-monospace font interface
 *
 * Used by ypaint text spans. No fixed cell size.
 * Each text span specifies its own font size — shader scales.
 * Font provides gpu_resource_set (atlas + glyph metadata).
 */

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/yrender/gpu-resource-set.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_font_font;

/* Font style */
enum yetty_font_style {
	YETTY_FONT_STYLE_REGULAR     = 0,
	YETTY_FONT_STYLE_BOLD        = 1,
	YETTY_FONT_STYLE_ITALIC      = 2,
	YETTY_FONT_STYLE_BOLD_ITALIC = 3,
};

YETTY_RESULT_DECLARE(yetty_font_font, struct yetty_font_font *);

/* Font ops */
struct yetty_font_font_ops {
	void (*destroy)(struct yetty_font_font *self);

	/* Glyph lookup — loads on demand, returns glyph index */
	struct uint32_result (*get_glyph_index)(struct yetty_font_font *self,
						uint32_t codepoint);
	struct uint32_result (*get_glyph_index_styled)(struct yetty_font_font *self,
						       uint32_t codepoint,
						       enum yetty_font_style style);

	/* Glyph loading */
	struct yetty_core_void_result (*load_glyphs)(struct yetty_font_font *self,
						     const uint32_t *codepoints,
						     size_t count);
	struct yetty_core_void_result (*load_basic_latin)(struct yetty_font_font *self);

	/* Base size the CDB was generated at */
	float (*get_base_size)(const struct yetty_font_font *self);

	/* Dirty tracking */
	int (*is_dirty)(const struct yetty_font_font *self);

	/* GPU resources — clears dirty internally */
	struct yetty_render_gpu_resource_set_result
	(*get_gpu_resource_set)(struct yetty_font_font *self);
};

/* Font base */
struct yetty_font_font {
	const struct yetty_font_font_ops *ops;
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_FONT_H */
