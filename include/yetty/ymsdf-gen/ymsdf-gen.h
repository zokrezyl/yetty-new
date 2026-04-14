#ifndef YETTY_YMSDF_GEN_H
#define YETTY_YMSDF_GEN_H

/*
 * ymsdf-gen - MSDF glyph generation interface
 *
 * Abstract interface for generating MSDF bitmaps from font glyphs.
 * Two implementations:
 *   - ymsdf-gen-cpu: uses msdfgen C++ library (CPU, multi-threaded)
 *   - ymsdf-gen-gpu: uses WebGPU compute shaders (GPU)
 *
 * Each implementation provides a yetty_ymsdf_gen that satisfies the ops vtable.
 */

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/*=============================================================================
 * Generated glyph — output from the generator
 *===========================================================================*/

struct yetty_ymsdf_gen_glyph {
	uint32_t codepoint;
	uint16_t width;          /* bitmap width in pixels */
	uint16_t height;         /* bitmap height in pixels */
	float bearing_x;
	float bearing_y;
	float advance;
	uint8_t *pixels;         /* RGBA8, width * height * 4 bytes, caller frees */
};

YETTY_RESULT_DECLARE(yetty_ymsdf_gen_glyph, struct yetty_ymsdf_gen_glyph);

/*=============================================================================
 * Font metrics — queried from the generator
 *===========================================================================*/

struct yetty_ymsdf_gen_metrics {
	float ascender;
	float descender;
	float max_advance;
	float units_per_em;
	int is_monospace;
};

YETTY_RESULT_DECLARE(yetty_ymsdf_gen_metrics, struct yetty_ymsdf_gen_metrics);

/*=============================================================================
 * Generator (opaque, polymorphic)
 *===========================================================================*/

struct yetty_ymsdf_gen;

struct yetty_ymsdf_gen_ops {
	void (*destroy)(struct yetty_ymsdf_gen *self);

	/* Get font metrics (scaled by font_size) */
	struct yetty_ymsdf_gen_metrics_result
	(*get_metrics)(const struct yetty_ymsdf_gen *self);

	/* Generate MSDF bitmap for a single glyph.
	 * Returns tight bitmap + bearing. Caller frees pixels.
	 */
	struct yetty_ymsdf_gen_glyph_result
	(*generate_glyph)(struct yetty_ymsdf_gen *self, uint32_t codepoint);
};

struct yetty_ymsdf_gen {
	const struct yetty_ymsdf_gen_ops *ops;
};

YETTY_RESULT_DECLARE(yetty_ymsdf_gen, struct yetty_ymsdf_gen *);

/*=============================================================================
 * CPU implementation (msdfgen library)
 *===========================================================================*/

struct yetty_ymsdf_gen_result
yetty_ymsdf_gen_cpu_create(const char *ttf_path,
			   float font_size,
			   float pixel_range);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMSDF_GEN_H */
