/*
 * ms-msdf-font.c - Monospace MSDF font implementation
 *
 * Reads .ms-cdb files (pre-composited fixed cells, bearing baked in).
 * Implements yetty_font_ms_font interface.
 * Grid atlas, shader computes UV from glyph_index + uniforms.
 */

#include <yetty/yfont/ms-msdf-font.h>
#include <yetty/yfont/ms-font.h>
#include <yetty/yms-msdf/yms-msdf.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/ycdb/ycdb.h>
#include <yetty/ycore/map.h>
#include <yetty/ytrace.h>
#include <webgpu/webgpu.h>

#include <stdlib.h>
#include <string.h>

#define ATLAS_COLS 32
#define ATLAS_MAX_DIM 8192
#define MAP_CAPACITY 8192

struct ms_msdf_font {
	struct yetty_font_ms_font base;

	struct yetty_ycdb_reader *cdb;

	uint32_t cell_width;
	uint32_t cell_height;
	float pixel_range;
	float font_size;

	uint8_t *atlas_pixels;
	uint32_t atlas_width;
	uint32_t atlas_height;
	uint32_t glyphs_per_row;
	uint32_t next_slot;

	struct yetty_core_map glyph_map;

	struct yetty_render_gpu_resource_set rs;
	int dirty;
};

/*=============================================================================
 * Helpers
 *===========================================================================*/

static void atlas_grow(struct ms_msdf_font *f)
{
	uint32_t new_h = f->atlas_height + f->cell_height;
	if (new_h > ATLAS_MAX_DIM)
		return;

	size_t old_sz = (size_t)f->atlas_width * f->atlas_height * 4;
	size_t new_sz = (size_t)f->atlas_width * new_h * 4;
	uint8_t *p = realloc(f->atlas_pixels, new_sz);
	if (!p)
		return;

	memset(p + old_sz, 0, new_sz - old_sz);
	f->atlas_pixels = p;
	f->atlas_height = new_h;
}

static struct uint32_result load_one(struct ms_msdf_font *f, uint32_t cp)
{
	const uint32_t *existing = yetty_core_map_get(&f->glyph_map, cp);
	if (existing)
		return YETTY_OK(uint32, *existing);

	uint32_t key = cp;
	void *data = NULL;
	size_t data_len = 0;

	struct yetty_core_void_result res =
		yetty_ycdb_reader_get(f->cdb, &key, sizeof(key), &data, &data_len);
	if (YETTY_IS_ERR(res))
		return YETTY_ERR(uint32, res.error.msg);
	if (!data)
		return YETTY_ERR(uint32, "glyph not found in ms-cdb");

	size_t expected = (size_t)f->cell_width * f->cell_height * 4;
	if (data_len != expected) {
		free(data);
		return YETTY_ERR(uint32, "glyph data size mismatch");
	}

	uint32_t slot = f->next_slot++;
	uint32_t col = slot % f->glyphs_per_row;
	uint32_t row = slot / f->glyphs_per_row;

	while ((row + 1) * f->cell_height > f->atlas_height)
		atlas_grow(f);

	uint32_t dx = col * f->cell_width;
	uint32_t dy = row * f->cell_height;

	for (uint32_t y = 0; y < f->cell_height; y++) {
		size_t dst = ((size_t)(dy + y) * f->atlas_width + dx) * 4;
		size_t src = (size_t)y * f->cell_width * 4;
		memcpy(f->atlas_pixels + dst, (uint8_t *)data + src,
		       f->cell_width * 4);
	}

	if (yetty_core_map_put(&f->glyph_map, cp, slot) < 0) {
		free(data);
		return YETTY_ERR(uint32, "glyph map full");
	}

	f->dirty = 1;
	free(data);
	return YETTY_OK(uint32, slot);
}

/*=============================================================================
 * Vtable
 *===========================================================================*/

static void ms_msdf_destroy(struct yetty_font_ms_font *self)
{
	struct ms_msdf_font *f = (struct ms_msdf_font *)self;
	if (!f)
		return;
	free(f->atlas_pixels);
	yetty_core_map_destroy(&f->glyph_map);
	yetty_ycdb_reader_close(f->cdb);
	free(f);
}

static struct pixel_size_result
ms_msdf_get_cell_size(const struct yetty_font_ms_font *self)
{
	const struct ms_msdf_font *f = (const struct ms_msdf_font *)self;
	if (!f)
		return YETTY_ERR(pixel_size, "font is NULL");
	struct pixel_size sz;
	sz.width = (float)f->cell_width;
	sz.height = (float)f->cell_height;
	return YETTY_OK(pixel_size, sz);
}

static struct uint32_result
ms_msdf_get_glyph_index(struct yetty_font_ms_font *self, uint32_t cp)
{
	struct ms_msdf_font *f = (struct ms_msdf_font *)self;
	if (!f)
		return YETTY_ERR(uint32, "font is NULL");
	return load_one(f, cp);
}

static struct uint32_result
ms_msdf_get_glyph_index_styled(struct yetty_font_ms_font *self,
				uint32_t cp, enum yetty_font_ms_style style)
{
	(void)style;
	return ms_msdf_get_glyph_index(self, cp);
}

static struct yetty_core_void_result
ms_msdf_resize(struct yetty_font_ms_font *self, float font_size)
{
	/* MSDF is resolution-independent — no-op */
	(void)self;
	(void)font_size;
	return YETTY_OK_VOID();
}

static struct yetty_core_void_result
ms_msdf_load_glyphs(struct yetty_font_ms_font *self,
		    const uint32_t *cps, size_t count)
{
	struct ms_msdf_font *f = (struct ms_msdf_font *)self;
	if (!f)
		return YETTY_ERR(yetty_core_void, "font is NULL");
	for (size_t i = 0; i < count; i++)
		load_one(f, cps[i]);
	return YETTY_OK_VOID();
}

static struct yetty_core_void_result
ms_msdf_load_basic_latin(struct yetty_font_ms_font *self)
{
	struct ms_msdf_font *f = (struct ms_msdf_font *)self;
	if (!f)
		return YETTY_ERR(yetty_core_void, "font is NULL");
	for (uint32_t cp = 0x20; cp <= 0x7E; cp++)
		load_one(f, cp);
	return YETTY_OK_VOID();
}

static int ms_msdf_is_dirty(const struct yetty_font_ms_font *self)
{
	return ((const struct ms_msdf_font *)self)->dirty;
}

static struct yetty_render_gpu_resource_set_result
ms_msdf_get_gpu_resource_set(struct yetty_font_ms_font *self)
{
	struct ms_msdf_font *f = (struct ms_msdf_font *)self;
	if (!f)
		return YETTY_ERR(yetty_render_gpu_resource_set, "font is NULL");

	if (f->dirty) {
		f->rs.textures[0].data = f->atlas_pixels;
		f->rs.textures[0].width = f->atlas_width;
		f->rs.textures[0].height = f->atlas_height;
		f->rs.textures[0].dirty = 1;

		f->rs.uniforms[0].u32 = f->glyphs_per_row;
		f->rs.uniforms[1].vec2[0] = (float)f->cell_width;
		f->rs.uniforms[1].vec2[1] = (float)f->cell_height;
		f->rs.uniforms[2].vec2[0] = (float)f->atlas_width;
		f->rs.uniforms[2].vec2[1] = (float)f->atlas_height;
		f->rs.uniforms[3].f32 = f->pixel_range;

		f->dirty = 0;
	}

	return YETTY_OK(yetty_render_gpu_resource_set, &f->rs);
}

static const struct yetty_font_ms_font_ops ms_msdf_ops = {
	.destroy = ms_msdf_destroy,
	.get_cell_size = ms_msdf_get_cell_size,
	.get_glyph_index = ms_msdf_get_glyph_index,
	.get_glyph_index_styled = ms_msdf_get_glyph_index_styled,
	.resize = ms_msdf_resize,
	.load_glyphs = ms_msdf_load_glyphs,
	.load_basic_latin = ms_msdf_load_basic_latin,
	.is_dirty = ms_msdf_is_dirty,
	.get_gpu_resource_set = ms_msdf_get_gpu_resource_set,
};

/*=============================================================================
 * Create
 *===========================================================================*/

struct yetty_font_ms_font_result
yetty_font_ms_msdf_font_create(const char *ms_cdb_path)
{
	if (!ms_cdb_path)
		return YETTY_ERR(yetty_font_ms_font, "ms_cdb_path is NULL");

	struct yetty_ycdb_reader_result cdb_res = yetty_ycdb_reader_open(ms_cdb_path);
	if (YETTY_IS_ERR(cdb_res))
		return YETTY_ERR(yetty_font_ms_font, cdb_res.error.msg);

	uint32_t meta_key = YETTY_YMS_MSDF_META_KEY;
	void *meta_data = NULL;
	size_t meta_len = 0;

	struct yetty_core_void_result get_res = yetty_ycdb_reader_get(
		cdb_res.value, &meta_key, sizeof(meta_key), &meta_data, &meta_len);
	if (YETTY_IS_ERR(get_res) || !meta_data ||
	    meta_len < sizeof(struct yetty_yms_msdf_meta)) {
		free(meta_data);
		yetty_ycdb_reader_close(cdb_res.value);
		return YETTY_ERR(yetty_font_ms_font, "invalid ms-cdb metadata");
	}

	struct yetty_yms_msdf_meta meta;
	memcpy(&meta, meta_data, sizeof(meta));
	free(meta_data);

	if (meta.cell_width == 0 || meta.cell_height == 0) {
		yetty_ycdb_reader_close(cdb_res.value);
		return YETTY_ERR(yetty_font_ms_font, "ms-cdb cell size is 0");
	}

	struct ms_msdf_font *f = calloc(1, sizeof(struct ms_msdf_font));
	if (!f) {
		yetty_ycdb_reader_close(cdb_res.value);
		return YETTY_ERR(yetty_font_ms_font, "allocation failed");
	}

	f->base.ops = &ms_msdf_ops;
	f->cdb = cdb_res.value;
	f->cell_width = meta.cell_width;
	f->cell_height = meta.cell_height;
	f->pixel_range = meta.pixel_range;
	f->font_size = meta.font_size;
	f->glyphs_per_row = ATLAS_COLS;
	f->next_slot = 1;
	f->dirty = 1;

	if (yetty_core_map_init(&f->glyph_map, MAP_CAPACITY) < 0) {
		yetty_ycdb_reader_close(f->cdb);
		free(f);
		return YETTY_ERR(yetty_font_ms_font, "map init failed");
	}

	f->atlas_width = f->cell_width * f->glyphs_per_row;
	f->atlas_height = f->cell_height;
	size_t atlas_bytes = (size_t)f->atlas_width * f->atlas_height * 4;
	f->atlas_pixels = calloc(atlas_bytes, 1);
	if (!f->atlas_pixels) {
		yetty_core_map_destroy(&f->glyph_map);
		yetty_ycdb_reader_close(f->cdb);
		free(f);
		return YETTY_ERR(yetty_font_ms_font, "atlas allocation failed");
	}

	strncpy(f->rs.namespace, "ms_msdf_font", YETTY_RENDER_NAME_MAX - 1);

	f->rs.texture_count = 1;
	struct yetty_render_texture *tex = &f->rs.textures[0];
	strncpy(tex->name, "texture", YETTY_RENDER_NAME_MAX - 1);
	strncpy(tex->wgsl_type, "texture_2d<f32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
	strncpy(tex->sampler_name, "sampler", YETTY_RENDER_NAME_MAX - 1);
	tex->format = WGPUTextureFormat_RGBA8Unorm;
	tex->sampler_filter = WGPUFilterMode_Linear;

	f->rs.uniform_count = 4;

	strncpy(f->rs.uniforms[0].name, "glyphs_per_row", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[0].type = YETTY_RENDER_UNIFORM_U32;

	strncpy(f->rs.uniforms[1].name, "glyph_cell_size", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[1].type = YETTY_RENDER_UNIFORM_VEC2;

	strncpy(f->rs.uniforms[2].name, "atlas_size", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[2].type = YETTY_RENDER_UNIFORM_VEC2;

	strncpy(f->rs.uniforms[3].name, "pixel_range", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[3].type = YETTY_RENDER_UNIFORM_F32;
	f->rs.uniforms[3].f32 = f->pixel_range;

	yinfo("ms_msdf_font: created from %s, cell=%ux%u pixel_range=%.1f",
	      ms_cdb_path, meta.cell_width, meta.cell_height, f->pixel_range);

	return YETTY_OK(yetty_font_ms_font, &f->base);
}
