#ifndef YETTY_FONT_MS_FONT_H
#define YETTY_FONT_MS_FONT_H

/*
 * yetty_font_ms_font - Monospace font interface
 *
 * Separate from yetty_font_font (non-monospace).
 * Used by terminal text-layer. Exposes get_cell_size.
 */

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/yrender/gpu-resource-set.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_font_ms_font;

/* Font style */
enum yetty_font_ms_style {
	YETTY_FONT_MS_STYLE_REGULAR     = 0,
	YETTY_FONT_MS_STYLE_BOLD        = 1,
	YETTY_FONT_MS_STYLE_ITALIC      = 2,
	YETTY_FONT_MS_STYLE_BOLD_ITALIC = 3,
};

YETTY_RESULT_DECLARE(yetty_font_ms_font, struct yetty_font_ms_font *);

struct yetty_font_ms_font_ops {
	void (*destroy)(struct yetty_font_ms_font *self);

	/* Cell size — the fixed cell dimensions for the grid */
	struct pixel_size_result (*get_cell_size)(const struct yetty_font_ms_font *self);

	/* Glyph lookup */
	struct uint32_result (*get_glyph_index)(struct yetty_font_ms_font *self,
						uint32_t codepoint);
	struct uint32_result (*get_glyph_index_styled)(struct yetty_font_ms_font *self,
						       uint32_t codepoint,
						       enum yetty_font_ms_style style);

	/* Resize — changes font size, recalculates cell size */
	struct yetty_core_void_result (*resize)(struct yetty_font_ms_font *self,
						float font_size);

	/* Glyph loading */
	struct yetty_core_void_result (*load_glyphs)(struct yetty_font_ms_font *self,
						     const uint32_t *codepoints,
						     size_t count);
	struct yetty_core_void_result (*load_basic_latin)(struct yetty_font_ms_font *self);

	/* Dirty tracking */
	int (*is_dirty)(const struct yetty_font_ms_font *self);

	/* GPU resources — clears dirty internally */
	struct yetty_render_gpu_resource_set_result
	(*get_gpu_resource_set)(struct yetty_font_ms_font *self);
};

struct yetty_font_ms_font {
	const struct yetty_font_ms_font_ops *ops;
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_MS_FONT_H */
