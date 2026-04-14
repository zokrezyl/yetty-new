#ifndef YETTY_YMS_MSDF_H
#define YETTY_YMS_MSDF_H

/*
 * yms-msdf - Monospace MSDF CDB generator
 *
 * Generates pre-composited MSDF glyph CDB files from TTF fonts.
 * Bearing is baked into fixed-size cells — no per-glyph metadata needed.
 * Output CDB is directly usable by the msdf-font for grid atlas packing.
 *
 * CDB format:
 *   key 0x00000000: cell metadata header (8 bytes: uint32 cell_w, uint32 cell_h)
 *   key <codepoint>: RGBA8 pixel data (cell_w * cell_h * 4 bytes)
 */

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/*=============================================================================
 * CDB metadata key (codepoint 0 is never a valid glyph)
 *===========================================================================*/

#define YETTY_YMS_MSDF_META_KEY 0x00000000

struct yetty_yms_msdf_meta {
	uint32_t cell_width;
	uint32_t cell_height;
	float pixel_range;
	float font_size;
};

/*=============================================================================
 * Generator config
 *===========================================================================*/

struct yetty_yms_msdf_config {
	const char *ttf_path;
	const char *output_path;
	float font_size;       /* font size in pixels (default 32) */
	float pixel_range;     /* MSDF pixel range (default 4) */
	int thread_count;      /* 0 = auto-detect */
	int include_nerd_fonts;
};

/*=============================================================================
 * API
 *===========================================================================*/

/* Generate monospace MSDF CDB from TTF font */
struct yetty_core_void_result
yetty_yms_msdf_generate(const struct yetty_yms_msdf_config *config);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMS_MSDF_H */
