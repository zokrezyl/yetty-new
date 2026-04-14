/*
 * msdf-font.c - Monospace MSDF font implementation
 *
 * Reads pre-generated MSDF glyph bitmaps from a CDB file.
 * Packs glyphs into a grid atlas (fixed cell size, monospace).
 * Shader computes UV from glyph_index + grid uniforms — no per-glyph buffer.
 */

#include <yetty/yfont/msdf-font.h>
#include <yetty/yfont/font.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/ytrace.h>
#include <webgpu/webgpu.h>

#include <yetty/ycdb/ycdb.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*=============================================================================
 * CDB glyph header — matches old MsdfGlyphData layout (28 bytes)
 *===========================================================================*/

struct msdf_glyph_header {
	uint32_t codepoint;
	uint16_t width;
	uint16_t height;
	float bearing_x;
	float bearing_y;
	float size_x;
	float size_y;
	float advance;
};

/*=============================================================================
 * Codepoint -> slot mapping
 *===========================================================================*/

#define MSDF_MAX_GLYPHS 4096
#define MSDF_ATLAS_INITIAL_COLS 32
#define MSDF_ATLAS_MAX_DIM 8192

struct codepoint_entry {
	uint32_t codepoint;
	uint32_t slot;
};

/*=============================================================================
 * Internal struct
 *===========================================================================*/

struct msdf_font {
	struct yetty_font_font base;

	/* CDB reader */
	struct yetty_ycdb_reader *cdb;

	/* Atlas */
	uint8_t *atlas_pixels;       /* RGBA8 */
	uint32_t atlas_width;
	uint32_t atlas_height;
	uint32_t cell_width;         /* glyph cell in atlas (from first loaded glyph) */
	uint32_t cell_height;
	uint32_t glyphs_per_row;
	uint32_t next_slot;          /* next free slot (0 = space/empty) */
	float pixel_range;

	/* Codepoint lookup */
	struct codepoint_entry entries[MSDF_MAX_GLYPHS];
	uint32_t entry_count;

	/* GPU resource set */
	struct yetty_render_gpu_resource_set rs;
	int dirty;
};

/*=============================================================================
 * Forward declarations
 *===========================================================================*/

static void msdf_font_destroy(struct yetty_font_font *self);
static enum yetty_font_render_method msdf_font_render_method(const struct yetty_font_font *self);
static uint32_t msdf_font_get_glyph_index(struct yetty_font_font *self, uint32_t codepoint);
static uint32_t msdf_font_get_glyph_index_styled(struct yetty_font_font *self,
						  uint32_t codepoint,
						  enum yetty_font_style style);
static void msdf_font_set_cell_size(struct yetty_font_font *self,
				    float cell_width, float cell_height);
static struct yetty_core_void_result msdf_font_load_glyphs(struct yetty_font_font *self,
							   const uint32_t *codepoints,
							   size_t count);
static struct yetty_core_void_result msdf_font_load_basic_latin(struct yetty_font_font *self);
static int msdf_font_is_dirty(const struct yetty_font_font *self);
static void msdf_font_clear_dirty(struct yetty_font_font *self);
static struct yetty_render_gpu_resource_set_result
msdf_font_get_gpu_resource_set(const struct yetty_font_font *self);

static const struct yetty_font_font_ops msdf_font_ops = {
	.destroy = msdf_font_destroy,
	.render_method = msdf_font_render_method,
	.get_glyph_index = msdf_font_get_glyph_index,
	.get_glyph_index_styled = msdf_font_get_glyph_index_styled,
	.set_cell_size = msdf_font_set_cell_size,
	.load_glyphs = msdf_font_load_glyphs,
	.load_basic_latin = msdf_font_load_basic_latin,
	.is_dirty = msdf_font_is_dirty,
	.clear_dirty = msdf_font_clear_dirty,
	.get_gpu_resource_set = msdf_font_get_gpu_resource_set,
};

/*=============================================================================
 * Helpers
 *===========================================================================*/

static uint32_t lookup_slot(const struct msdf_font *font, uint32_t codepoint)
{
	for (uint32_t i = 0; i < font->entry_count; i++) {
		if (font->entries[i].codepoint == codepoint)
			return font->entries[i].slot;
	}
	return 0;
}

static void atlas_grow(struct msdf_font *font)
{
	uint32_t new_height = font->atlas_height + font->cell_height;
	if (new_height > MSDF_ATLAS_MAX_DIM)
		return;

	size_t old_size = (size_t)font->atlas_width * font->atlas_height * 4;
	size_t new_size = (size_t)font->atlas_width * new_height * 4;

	uint8_t *new_pixels = realloc(font->atlas_pixels, new_size);
	if (!new_pixels)
		return;

	memset(new_pixels + old_size, 0, new_size - old_size);
	font->atlas_pixels = new_pixels;
	font->atlas_height = new_height;
}

static uint32_t load_one_glyph(struct msdf_font *font, uint32_t codepoint)
{
	/* Already loaded? */
	uint32_t existing = lookup_slot(font, codepoint);
	if (existing)
		return existing;

	if (font->next_slot >= MSDF_MAX_GLYPHS)
		return 0;

	/* Look up in CDB */
	uint32_t key = codepoint;
	void *data = NULL;
	size_t data_len = 0;

	struct yetty_core_void_result get_res =
		yetty_ycdb_reader_get(font->cdb, &key, sizeof(key), &data, &data_len);
	if (YETTY_IS_ERR(get_res) || !data)
		return 0;

	if (data_len < sizeof(struct msdf_glyph_header)) {
		free(data);
		return 0;
	}

	struct msdf_glyph_header hdr;
	memcpy(&hdr, data, sizeof(hdr));
	const uint8_t *pixels = (const uint8_t *)data + sizeof(hdr);
	uint32_t pixel_size = (uint32_t)hdr.width * hdr.height * 4;

	if (data_len < sizeof(hdr) + pixel_size) {
		free(data);
		return 0;
	}

	/* Atlas not yet allocated — need scan_max_cell_size first */
	if (!font->atlas_pixels) {
		free(data);
		return 0;
	}

	/* Skip glyphs larger than cell (shouldn't happen after scan) */
	if (hdr.width > font->cell_width || hdr.height > font->cell_height) {
		ydebug("msdf_font: glyph U+%04X size %ux%u > cell %ux%u, skipping",
		       codepoint, hdr.width, hdr.height,
		       font->cell_width, font->cell_height);
		free(data);
		return 0;
	}

	/* Allocate slot */
	uint32_t slot = font->next_slot++;
	uint32_t col = slot % font->glyphs_per_row;
	uint32_t row = slot / font->glyphs_per_row;

	/* Grow atlas if needed */
	while ((row + 1) * font->cell_height > font->atlas_height)
		atlas_grow(font);

	/* Copy glyph pixels into atlas */
	uint32_t dst_x = col * font->cell_width;
	uint32_t dst_y = row * font->cell_height;

	for (uint32_t y = 0; y < hdr.height; y++) {
		size_t dst_offset = ((dst_y + y) * font->atlas_width + dst_x) * 4;
		size_t src_offset = y * hdr.width * 4;
		memcpy(font->atlas_pixels + dst_offset, pixels + src_offset,
		       hdr.width * 4);
	}

	/* Record mapping */
	font->entries[font->entry_count].codepoint = codepoint;
	font->entries[font->entry_count].slot = slot;
	font->entry_count++;

	font->dirty = 1;
	free(data);
	return slot;
}

/*=============================================================================
 * Vtable implementation
 *===========================================================================*/

static void msdf_font_destroy(struct yetty_font_font *self)
{
	struct msdf_font *font = (struct msdf_font *)self;
	if (!font)
		return;
	free(font->atlas_pixels);
	yetty_ycdb_reader_close(font->cdb);
	free(font);
}

static enum yetty_font_render_method
msdf_font_render_method(const struct yetty_font_font *self)
{
	(void)self;
	return YETTY_FONT_RENDER_METHOD_MSDF;
}

static uint32_t msdf_font_get_glyph_index(struct yetty_font_font *self,
					   uint32_t codepoint)
{
	struct msdf_font *font = (struct msdf_font *)self;
	uint32_t slot = lookup_slot(font, codepoint);
	if (slot)
		return slot;
	/* Try to load on demand */
	return load_one_glyph(font, codepoint);
}

static uint32_t msdf_font_get_glyph_index_styled(struct yetty_font_font *self,
						  uint32_t codepoint,
						  enum yetty_font_style style)
{
	/* For now, ignore style — single CDB has one style */
	(void)style;
	return msdf_font_get_glyph_index(self, codepoint);
}

static void msdf_font_set_cell_size(struct yetty_font_font *self,
				    float cell_width, float cell_height)
{
	/* MSDF is resolution-independent — cell size is informational only */
	(void)self;
	(void)cell_width;
	(void)cell_height;
}

static struct yetty_core_void_result
msdf_font_load_glyphs(struct yetty_font_font *self,
		       const uint32_t *codepoints, size_t count)
{
	struct msdf_font *font = (struct msdf_font *)self;
	if (!font)
		return YETTY_ERR(yetty_core_void, "font is NULL");

	for (size_t i = 0; i < count; i++)
		load_one_glyph(font, codepoints[i]);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result
msdf_font_load_basic_latin(struct yetty_font_font *self)
{
	struct msdf_font *font = (struct msdf_font *)self;
	if (!font)
		return YETTY_ERR(yetty_core_void, "font is NULL");

	for (uint32_t cp = 0x20; cp <= 0x7E; cp++)
		load_one_glyph(font, cp);

	return YETTY_OK_VOID();
}

static int msdf_font_is_dirty(const struct yetty_font_font *self)
{
	const struct msdf_font *font = (const struct msdf_font *)self;
	return font->dirty;
}

static void msdf_font_clear_dirty(struct yetty_font_font *self)
{
	struct msdf_font *font = (struct msdf_font *)self;
	font->dirty = 0;
}

static struct yetty_render_gpu_resource_set_result
msdf_font_get_gpu_resource_set(const struct yetty_font_font *self)
{
	struct msdf_font *font = (struct msdf_font *)self;

	if (font->dirty) {
		/* Update atlas texture */
		struct yetty_render_texture *tex = &font->rs.textures[0];
		tex->data = font->atlas_pixels;
		tex->width = font->atlas_width;
		tex->height = font->atlas_height;
		tex->dirty = 1;

		/* Update uniforms */
		font->rs.uniforms[0].u32 = font->glyphs_per_row;
		font->rs.uniforms[1].vec2[0] = (float)font->cell_width;
		font->rs.uniforms[1].vec2[1] = (float)font->cell_height;
		font->rs.uniforms[2].vec2[0] = (float)font->atlas_width;
		font->rs.uniforms[2].vec2[1] = (float)font->atlas_height;
		font->rs.uniforms[3].f32 = font->pixel_range;
	}

	return YETTY_OK(yetty_render_gpu_resource_set, &font->rs);
}

/*=============================================================================
 * Create
 *===========================================================================*/

struct yetty_font_font_result
yetty_font_msdf_font_create(const char *cdb_path, float pixel_range)
{
	if (!cdb_path)
		return YETTY_ERR(yetty_font_font, "cdb_path is NULL");

	struct yetty_ycdb_reader_result cdb_res = yetty_ycdb_reader_open(cdb_path);
	if (YETTY_IS_ERR(cdb_res))
		return YETTY_ERR(yetty_font_font, cdb_res.error.msg);

	struct msdf_font *font = calloc(1, sizeof(struct msdf_font));
	if (!font) {
		yetty_ycdb_reader_close(cdb_res.value);
		return YETTY_ERR(yetty_font_font, "allocation failed");
	}

	font->base.ops = &msdf_font_ops;
	font->cdb = cdb_res.value;
	font->pixel_range = pixel_range;
	font->next_slot = 1; /* slot 0 = empty/space */
	font->dirty = 1;

	/* GPU resource set */
	strncpy(font->rs.namespace, "msdf_font", YETTY_RENDER_NAME_MAX - 1);

	/* Texture: RGBA8 atlas */
	font->rs.texture_count = 1;
	struct yetty_render_texture *tex = &font->rs.textures[0];
	strncpy(tex->name, "texture", YETTY_RENDER_NAME_MAX - 1);
	strncpy(tex->wgsl_type, "texture_2d<f32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
	strncpy(tex->sampler_name, "sampler", YETTY_RENDER_NAME_MAX - 1);
	tex->format = WGPUTextureFormat_RGBA8Unorm;
	tex->sampler_filter = WGPUFilterMode_Linear;

	/* Uniforms: grid layout + pixel_range */
	font->rs.uniform_count = 4;

	struct yetty_render_uniform *u;

	u = &font->rs.uniforms[0];
	strncpy(u->name, "glyphs_per_row", YETTY_RENDER_NAME_MAX - 1);
	u->type = YETTY_RENDER_UNIFORM_U32;

	u = &font->rs.uniforms[1];
	strncpy(u->name, "glyph_cell_size", YETTY_RENDER_NAME_MAX - 1);
	u->type = YETTY_RENDER_UNIFORM_VEC2;

	u = &font->rs.uniforms[2];
	strncpy(u->name, "atlas_size", YETTY_RENDER_NAME_MAX - 1);
	u->type = YETTY_RENDER_UNIFORM_VEC2;

	u = &font->rs.uniforms[3];
	strncpy(u->name, "pixel_range", YETTY_RENDER_NAME_MAX - 1);
	u->type = YETTY_RENDER_UNIFORM_F32;
	u->f32 = pixel_range;

	yinfo("msdf_font: created from %s, pixel_range=%.1f", cdb_path, pixel_range);

	return YETTY_OK(yetty_font_font, &font->base);
}
