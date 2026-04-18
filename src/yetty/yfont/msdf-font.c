/*
 * msdf-font.c - Non-monospace MSDF font implementation
 *
 * Reads .cdb files (glyph header + RGBA bitmap per glyph).
 * Uses uniform cell grid atlas — all glyphs in same-size cells.
 * UV computed from slot index; bearing/advance handle layout.
 * Implements yetty_font_font interface (non-monospace).
 * Can be used at any font size — shader scales from base_size.
 */

#include <yetty/yfont/msdf-font.h>
#include <yetty/yfont/font.h>
#include <yetty/ymsdf-gen/ymsdf-gen.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/ycdb/ycdb.h>
#include <yetty/ycore/map.h>
#include <yetty/ytrace.h>
#include <webgpu/webgpu.h>

#include <stdlib.h>
#include <string.h>

#define INCBIN_STYLE 1
#include <incbin.h>
INCBIN(msdf_font_shader, YETTY_YFONT_SHADER_DIR "/msdf-font.wgsl");

#define ATLAS_INITIAL_W 1024
#define ATLAS_INITIAL_H 512
#define ATLAS_MAX_DIM 16384
#define ATLAS_PADDING 2
#define MAP_CAPACITY 8192

/* Per-glyph GPU metadata (24 bytes, 6 floats)
 * cell_idx stores the actual atlas cell (may differ from slot due to empty glyphs).
 */
struct glyph_meta_gpu {
	float size_x, size_y;       /* logical glyph size */
	float bearing_x, bearing_y; /* offset from pen position */
	float advance;              /* horizontal advance */
	float cell_idx;             /* atlas cell index for UV computation */
};

struct msdf_font {
	struct yetty_font_font base;

	struct yetty_ycdb_reader *cdb;

	uint8_t *atlas_pixels;
	uint32_t atlas_width;
	uint32_t atlas_height;

	/* Uniform cell grid */
	uint32_t cell_size;   /* uniform cell size (square) */
	uint32_t atlas_cols;  /* cells per row */
	uint32_t next_cell;   /* next cell index to use */

	struct glyph_meta_gpu *meta;
	uint32_t meta_capacity;
	uint32_t next_slot;

	struct yetty_core_map glyph_map;

	float base_size; /* font size CDB was generated at */
	float pixel_range;

	struct yetty_render_gpu_resource_set rs;
	int dirty;
};

/*=============================================================================
 * Atlas helpers
 *===========================================================================*/

static void atlas_grow(struct msdf_font *f)
{
	/* Add more rows */
	uint32_t rows_to_add = 4;
	uint32_t new_h = f->atlas_height + rows_to_add * f->cell_size;
	if (new_h > ATLAS_MAX_DIM) return;

	size_t old_sz = (size_t)f->atlas_width * f->atlas_height * 4;
	size_t new_sz = (size_t)f->atlas_width * new_h * 4;
	uint8_t *p = realloc(f->atlas_pixels, new_sz);
	if (!p) return;

	memset(p + old_sz, 0, new_sz - old_sz);
	f->atlas_pixels = p;
	f->atlas_height = new_h;
}

static struct uint32_result load_one(struct msdf_font *f, uint32_t cp)
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
		return YETTY_ERR(uint32, "glyph not found");

	if (data_len < sizeof(struct yetty_ymsdf_gen_glyph_header)) {
		free(data);
		return YETTY_ERR(uint32, "glyph data too small");
	}

	struct yetty_ymsdf_gen_glyph_header hdr;
	memcpy(&hdr, data, sizeof(hdr));
	const uint8_t *pixels = (const uint8_t *)data + sizeof(hdr);
	size_t pixel_bytes = (size_t)hdr.width * hdr.height * 4;

	if (data_len < sizeof(hdr) + pixel_bytes) {
		free(data);
		return YETTY_ERR(uint32, "glyph pixel data truncated");
	}

	uint32_t slot = f->next_slot;

	if (slot >= f->meta_capacity) {
		uint32_t new_cap = f->meta_capacity * 2;
		struct glyph_meta_gpu *new_meta = realloc(f->meta,
			new_cap * sizeof(struct glyph_meta_gpu));
		if (!new_meta) {
			free(data);
			return YETTY_ERR(uint32, "meta realloc failed");
		}
		f->meta = new_meta;
		f->meta_capacity = new_cap;
	}

	/* Empty glyph (space) — just metadata, no atlas pixels */
	if (hdr.width == 0 || hdr.height == 0) {
		struct glyph_meta_gpu *m = &f->meta[slot];
		memset(m, 0, sizeof(*m));
		m->advance = hdr.advance;
		m->cell_idx = -1.0f;  /* No atlas cell for empty glyphs */
		f->next_slot++;
		yetty_core_map_put(&f->glyph_map, cp, slot);
		f->dirty = 1;
		free(data);
		return YETTY_OK(uint32, slot);
	}

	/* Place in uniform cell grid */
	uint32_t cell_idx = f->next_cell;
	uint32_t col = cell_idx % f->atlas_cols;
	uint32_t row = cell_idx / f->atlas_cols;

	/* Grow atlas if needed */
	while ((row + 1) * f->cell_size > f->atlas_height)
		atlas_grow(f);

	uint32_t ax = col * f->cell_size;
	uint32_t ay = row * f->cell_size;

	/* Center glyph in cell */
	uint32_t gw = hdr.width;
	uint32_t gh = hdr.height;
	uint32_t ox = (f->cell_size - gw) / 2;
	uint32_t oy = (f->cell_size - gh) / 2;

	/* Copy pixels into atlas cell */
	for (uint32_t y = 0; y < gh; y++) {
		size_t dst = ((size_t)(ay + oy + y) * f->atlas_width + ax + ox) * 4;
		size_t src = (size_t)y * gw * 4;
		memcpy(f->atlas_pixels + dst, pixels + src, gw * 4);
	}

	f->next_cell++;

	/* Fill metadata — cell_idx used by shader for UV computation */
	struct glyph_meta_gpu *m = &f->meta[slot];
	m->size_x = hdr.size_x;
	m->size_y = hdr.size_y;
	m->bearing_x = hdr.bearing_x;
	m->bearing_y = hdr.bearing_y;
	m->advance = hdr.advance;
	m->cell_idx = (float)cell_idx;

	f->next_slot++;
	yetty_core_map_put(&f->glyph_map, cp, slot);
	f->dirty = 1;
	free(data);
	return YETTY_OK(uint32, slot);
}

/*=============================================================================
 * Vtable
 *===========================================================================*/

static void msdf_destroy(struct yetty_font_font *self)
{
	struct msdf_font *f = (struct msdf_font *)self;
	if (!f) return;
	free(f->atlas_pixels);
	free(f->meta);
	yetty_core_map_destroy(&f->glyph_map);
	yetty_ycdb_reader_close(f->cdb);
	free(f);
}

static struct uint32_result
msdf_get_glyph_index(struct yetty_font_font *self, uint32_t cp)
{
	struct msdf_font *f = (struct msdf_font *)self;
	if (!f) return YETTY_ERR(uint32, "font is NULL");
	return load_one(f, cp);
}

static struct uint32_result
msdf_get_glyph_index_styled(struct yetty_font_font *self,
			    uint32_t cp, enum yetty_font_style style)
{
	(void)style;
	return msdf_get_glyph_index(self, cp);
}

static struct yetty_core_void_result
msdf_load_glyphs(struct yetty_font_font *self,
		 const uint32_t *cps, size_t count)
{
	struct msdf_font *f = (struct msdf_font *)self;
	if (!f) return YETTY_ERR(yetty_core_void, "font is NULL");
	for (size_t i = 0; i < count; i++)
		load_one(f, cps[i]);
	return YETTY_OK_VOID();
}

static struct yetty_core_void_result
msdf_load_basic_latin(struct yetty_font_font *self)
{
	struct msdf_font *f = (struct msdf_font *)self;
	if (!f) return YETTY_ERR(yetty_core_void, "font is NULL");
	for (uint32_t cp = 0x20; cp <= 0x7E; cp++)
		load_one(f, cp);
	return YETTY_OK_VOID();
}

static float msdf_get_base_size(const struct yetty_font_font *self)
{
	const struct msdf_font *f = (const struct msdf_font *)self;
	return f ? f->base_size : 32.0f;
}

static int msdf_is_dirty(const struct yetty_font_font *self)
{
	return ((const struct msdf_font *)self)->dirty;
}

static struct yetty_render_gpu_resource_set_result
msdf_get_gpu_resource_set(struct yetty_font_font *self)
{
	struct msdf_font *f = (struct msdf_font *)self;
	if (!f) return YETTY_ERR(yetty_render_gpu_resource_set, "font is NULL");

	if (f->dirty) {
		f->rs.textures[0].data = f->atlas_pixels;
		f->rs.textures[0].width = f->atlas_width;
		f->rs.textures[0].height = f->atlas_height;
		f->rs.textures[0].dirty = 1;

		f->rs.buffers[0].data = (uint8_t *)f->meta;
		f->rs.buffers[0].size = (size_t)f->next_slot * sizeof(struct glyph_meta_gpu);
		f->rs.buffers[0].dirty = 1;

		f->rs.uniforms[0].f32 = f->pixel_range;
		f->rs.uniforms[1].f32 = f->base_size;
		f->rs.uniforms[2].u32 = f->cell_size;
		f->rs.uniforms[3].u32 = f->atlas_cols;

		f->dirty = 0;
	}

	return YETTY_OK(yetty_render_gpu_resource_set, &f->rs);
}

static const struct yetty_font_font_ops msdf_font_ops = {
	.destroy = msdf_destroy,
	.get_glyph_index = msdf_get_glyph_index,
	.get_glyph_index_styled = msdf_get_glyph_index_styled,
	.load_glyphs = msdf_load_glyphs,
	.load_basic_latin = msdf_load_basic_latin,
	.get_base_size = msdf_get_base_size,
	.is_dirty = msdf_is_dirty,
	.get_gpu_resource_set = msdf_get_gpu_resource_set,
};

/*=============================================================================
 * Create
 *===========================================================================*/

#define DEFAULT_CELL_SIZE 64

struct yetty_font_font_result
yetty_font_msdf_font_create(const char *cdb_path)
{
	if (!cdb_path)
		return YETTY_ERR(yetty_font_font, "cdb_path is NULL");

	ydebug("msdf_font: opening %s", cdb_path);

	struct yetty_ycdb_reader_result cdb_res = yetty_ycdb_reader_open(cdb_path);
	if (YETTY_IS_ERR(cdb_res))
		return YETTY_ERR(yetty_font_font, cdb_res.error.msg);

	struct msdf_font *f = calloc(1, sizeof(struct msdf_font));
	if (!f) {
		yetty_ycdb_reader_close(cdb_res.value);
		return YETTY_ERR(yetty_font_font, "allocation failed");
	}

	f->base.ops = &msdf_font_ops;
	f->cdb = cdb_res.value;
	f->base_size = 32.0f; /* TODO: read from CDB metadata */
	f->pixel_range = 4.0f;
	f->dirty = 1;

	/* Uniform cell grid */
	f->cell_size = DEFAULT_CELL_SIZE;
	f->atlas_width = ATLAS_INITIAL_W;
	f->atlas_cols = f->atlas_width / f->cell_size;
	f->atlas_height = ATLAS_INITIAL_H;
	f->next_cell = 0;

	size_t atlas_bytes = (size_t)f->atlas_width * f->atlas_height * 4;
	f->atlas_pixels = calloc(atlas_bytes, 1);
	if (!f->atlas_pixels) {
		yetty_ycdb_reader_close(f->cdb);
		free(f);
		return YETTY_ERR(yetty_font_font, "atlas allocation failed");
	}

	f->meta_capacity = 256;
	f->meta = calloc(f->meta_capacity, sizeof(struct glyph_meta_gpu));
	if (!f->meta) {
		free(f->atlas_pixels);
		yetty_ycdb_reader_close(f->cdb);
		free(f);
		return YETTY_ERR(yetty_font_font, "meta allocation failed");
	}
	f->next_slot = 1;

	if (yetty_core_map_init(&f->glyph_map, MAP_CAPACITY) < 0) {
		free(f->meta);
		free(f->atlas_pixels);
		yetty_ycdb_reader_close(f->cdb);
		free(f);
		return YETTY_ERR(yetty_font_font, "map init failed");
	}

	/* GPU resource set */
	strncpy(f->rs.namespace, "msdf_font", YETTY_RENDER_NAME_MAX - 1);

	f->rs.texture_count = 1;
	struct yetty_render_texture *tex = &f->rs.textures[0];
	strncpy(tex->name, "texture", YETTY_RENDER_NAME_MAX - 1);
	strncpy(tex->wgsl_type, "texture_2d<f32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
	strncpy(tex->sampler_name, "sampler", YETTY_RENDER_NAME_MAX - 1);
	tex->format = WGPUTextureFormat_RGBA8Unorm;
	tex->sampler_filter = WGPUFilterMode_Linear;

	f->rs.buffer_count = 1;
	struct yetty_render_buffer *buf = &f->rs.buffers[0];
	strncpy(buf->name, "buffer", YETTY_RENDER_NAME_MAX - 1);
	strncpy(buf->wgsl_type, "array<f32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
	buf->readonly = 1;

	f->rs.uniform_count = 4;
	strncpy(f->rs.uniforms[0].name, "pixel_range", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[0].type = YETTY_RENDER_UNIFORM_F32;
	f->rs.uniforms[0].f32 = f->pixel_range;

	strncpy(f->rs.uniforms[1].name, "base_size", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[1].type = YETTY_RENDER_UNIFORM_F32;
	f->rs.uniforms[1].f32 = f->base_size;

	strncpy(f->rs.uniforms[2].name, "cell_size", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[2].type = YETTY_RENDER_UNIFORM_U32;
	f->rs.uniforms[2].u32 = f->cell_size;

	strncpy(f->rs.uniforms[3].name, "atlas_cols", YETTY_RENDER_NAME_MAX - 1);
	f->rs.uniforms[3].type = YETTY_RENDER_UNIFORM_U32;
	f->rs.uniforms[3].u32 = f->atlas_cols;

	yetty_render_shader_code_set(&f->rs.shader,
		(const char *)gmsdf_font_shader_data, gmsdf_font_shader_size);

	yinfo("msdf_font: created from %s, base_size=%.0f", cdb_path, f->base_size);

	return YETTY_OK(yetty_font_font, &f->base);
}
