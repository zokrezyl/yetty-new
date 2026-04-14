/*
 * yms-msdf - Monospace MSDF CDB generator (pure C)
 *
 * Uses ymsdf-gen interface to generate MSDF bitmaps,
 * composites into fixed cells with bearing baked in,
 * writes to CDB via ycdb.
 */

#include <yetty/yms-msdf/yms-msdf.h>
#include <yetty/ymsdf-gen/ymsdf-gen.h>
#include <yetty/ycdb/ycdb.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*=============================================================================
 * Default charset
 *===========================================================================*/

static size_t add_range(uint32_t *buf, size_t pos, size_t cap,
			uint32_t lo, uint32_t hi)
{
	for (uint32_t c = lo; c <= hi && pos < cap; c++)
		buf[pos++] = c;
	return pos;
}

#define CS_CAP 131072

static size_t default_charset(uint32_t *buf, size_t cap, int nerd_fonts)
{
	size_t n = 0;
	n = add_range(buf, n, cap, 0x20, 0x7E);
	n = add_range(buf, n, cap, 0xA0, 0xFF);
	n = add_range(buf, n, cap, 0x100, 0x17F);
	n = add_range(buf, n, cap, 0x180, 0x24F);
	n = add_range(buf, n, cap, 0x370, 0x3FF);
	n = add_range(buf, n, cap, 0x400, 0x4FF);
	n = add_range(buf, n, cap, 0x2000, 0x206F);
	n = add_range(buf, n, cap, 0x2190, 0x21FF);
	n = add_range(buf, n, cap, 0x2500, 0x257F);
	n = add_range(buf, n, cap, 0x2580, 0x259F);
	n = add_range(buf, n, cap, 0x25A0, 0x25FF);

	if (nerd_fonts) {
		n = add_range(buf, n, cap, 0xE0A0, 0xE0D7);
		n = add_range(buf, n, cap, 0xE5FA, 0xE6AC);
		n = add_range(buf, n, cap, 0xE700, 0xE7C5);
		n = add_range(buf, n, cap, 0xE200, 0xE2A9);
		n = add_range(buf, n, cap, 0xE300, 0xE3E3);
		n = add_range(buf, n, cap, 0xF400, 0xF532);
		n = add_range(buf, n, cap, 0xEA60, 0xEBEB);
		n = add_range(buf, n, cap, 0xF0001, 0xF1AF0);
	}

	return n;
}

/*=============================================================================
 * Public API
 *===========================================================================*/

struct yetty_core_void_result
yetty_yms_msdf_generate(const struct yetty_yms_msdf_config *config)
{
	if (!config || !config->ttf_path || !config->output_path)
		return YETTY_ERR(yetty_core_void, "invalid config");

	float font_size = config->font_size > 0 ? config->font_size : 32.0f;
	float pixel_range = config->pixel_range > 0 ? config->pixel_range : 4.0f;

	/* Create generator */
	struct yetty_ymsdf_gen_result gen_res =
		yetty_ymsdf_gen_cpu_create(config->ttf_path, font_size, pixel_range);
	if (YETTY_IS_ERR(gen_res))
		return YETTY_ERR(yetty_core_void, gen_res.error.msg);
	struct yetty_ymsdf_gen *gen = gen_res.value;

	/* Get font metrics to compute cell size */
	struct yetty_ymsdf_gen_metrics_result met_res = gen->ops->get_metrics(gen);
	if (YETTY_IS_ERR(met_res)) {
		gen->ops->destroy(gen);
		return YETTY_ERR(yetty_core_void, met_res.error.msg);
	}
	struct yetty_ymsdf_gen_metrics met = met_res.value;

	int pad = (int)ceilf(pixel_range);
	int cell_h = (int)ceilf(met.ascender - met.descender) + pad * 2;
	int cell_w = (int)ceilf(met.max_advance) + pad * 2;
	int baseline = (int)ceilf(met.ascender) + pad;

	if (cell_w <= 0 || cell_h <= 0) {
		gen->ops->destroy(gen);
		return YETTY_ERR(yetty_core_void, "invalid cell dimensions");
	}

	/* Build charset */
	uint32_t *charset = malloc(CS_CAP * sizeof(uint32_t));
	if (!charset) {
		gen->ops->destroy(gen);
		return YETTY_ERR(yetty_core_void, "allocation failed");
	}
	size_t cs_count = default_charset(charset, CS_CAP,
					  config->include_nerd_fonts);

	/* Open CDB writer */
	struct yetty_ycdb_writer_result wr = yetty_ycdb_writer_create(config->output_path);
	if (YETTY_IS_ERR(wr)) {
		free(charset);
		gen->ops->destroy(gen);
		return YETTY_ERR(yetty_core_void, wr.error.msg);
	}

	/* Write metadata at key 0 */
	struct yetty_yms_msdf_meta meta = {0};
	meta.cell_width = (uint32_t)cell_w;
	meta.cell_height = (uint32_t)cell_h;
	meta.pixel_range = pixel_range;
	meta.font_size = font_size;

	uint32_t meta_key = YETTY_YMS_MSDF_META_KEY;
	struct yetty_core_void_result add_res = yetty_ycdb_writer_add(
		wr.value, &meta_key, sizeof(meta_key), &meta, sizeof(meta));
	if (YETTY_IS_ERR(add_res)) {
		yetty_ycdb_writer_finish(wr.value);
		free(charset);
		gen->ops->destroy(gen);
		return add_res;
	}

	/* Generate and composite each glyph */
	size_t cell_bytes = (size_t)cell_w * cell_h * 4;
	uint8_t *cell = calloc(cell_bytes, 1);
	if (!cell) {
		yetty_ycdb_writer_finish(wr.value);
		free(charset);
		gen->ops->destroy(gen);
		return YETTY_ERR(yetty_core_void, "allocation failed");
	}

	size_t written = 0;
	for (size_t i = 0; i < cs_count; i++) {
		uint32_t cp = charset[i];

		struct yetty_ymsdf_gen_glyph_result gr =
			gen->ops->generate_glyph(gen, cp);
		if (YETTY_IS_ERR(gr))
			continue;

		struct yetty_ymsdf_gen_glyph g = gr.value;

		/* Clear cell */
		memset(cell, 0, cell_bytes);

		/* Composite bitmap into cell with bearing */
		if (g.pixels && g.width > 0 && g.height > 0) {
			int off_x = (int)roundf(g.bearing_x) + pad;
			if (off_x < 0) off_x = 0;
			if (off_x + g.width > cell_w)
				off_x = cell_w - g.width;
			if (off_x < 0) off_x = 0;

			int off_y = baseline - (int)roundf(g.bearing_y) + pad;
			if (off_y < 0) off_y = 0;
			if (off_y + g.height > cell_h)
				off_y = cell_h - g.height;
			if (off_y < 0) off_y = 0;

			for (int y = 0; y < g.height && (off_y + y) < cell_h; y++) {
				size_t dst_off = ((size_t)(off_y + y) * cell_w + off_x) * 4;
				size_t src_off = (size_t)y * g.width * 4;
				size_t row_bytes = (size_t)g.width * 4;
				if (off_x + g.width > cell_w)
					row_bytes = (size_t)(cell_w - off_x) * 4;
				memcpy(cell + dst_off, g.pixels + src_off, row_bytes);
			}
		}

		free(g.pixels);

		/* Write to CDB */
		add_res = yetty_ycdb_writer_add(wr.value, &cp, sizeof(cp),
						cell, cell_bytes);
		if (YETTY_IS_ERR(add_res))
			continue;
		written++;
	}

	free(cell);
	free(charset);
	gen->ops->destroy(gen);

	struct yetty_core_void_result finish_res = yetty_ycdb_writer_finish(wr.value);
	if (YETTY_IS_ERR(finish_res))
		return finish_res;

	return YETTY_OK_VOID();
}
