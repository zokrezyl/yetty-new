/*
 * ymsdf-gen-cpu - CPU MSDF glyph generator using msdfgen library
 *
 * C++ implementation file — exposes C API via ymsdf-gen.h vtable.
 */

#include <yetty/ymsdf-gen/ymsdf-gen.h>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

struct yetty_ymsdf_gen_cpu {
	struct yetty_ymsdf_gen base;
	msdfgen::FreetypeHandle *ft;
	msdfgen::FontHandle *font;
	float font_size;
	float pixel_range;
	double font_scale;
	msdfgen::FontMetrics metrics;
};

static void cpu_destroy(struct yetty_ymsdf_gen *self)
{
	auto *gen = reinterpret_cast<struct yetty_ymsdf_gen_cpu *>(self);
	if (!gen)
		return;
	if (gen->font)
		msdfgen::destroyFont(gen->font);
	if (gen->ft)
		msdfgen::deinitializeFreetype(gen->ft);
	free(gen);
}

static struct yetty_ymsdf_gen_metrics_result
cpu_get_metrics(const struct yetty_ymsdf_gen *self)
{
	auto *gen = reinterpret_cast<const struct yetty_ymsdf_gen_cpu *>(self);

	struct yetty_ymsdf_gen_metrics m;
	memset(&m, 0, sizeof(m));
	m.ascender = (float)(gen->metrics.ascenderY * gen->font_scale);
	m.descender = (float)(gen->metrics.descenderY * gen->font_scale);
	m.units_per_em = (float)gen->metrics.emSize;
	m.is_monospace = 0;

	/* Get max advance via FreeType directly */
	msdfgen::FontMetrics fm;
	msdfgen::getFontMetrics(fm, gen->font);
	/* msdfgen doesn't expose max_advance directly, estimate from 'M' */
	double advance = 0;
	msdfgen::Shape shape;
	if (msdfgen::loadGlyph(shape, gen->font, 'M', &advance))
		m.max_advance = (float)(advance * gen->font_scale);

	return YETTY_OK(yetty_ymsdf_gen_metrics, m);
}

static struct yetty_ymsdf_gen_glyph_result
cpu_generate_glyph(struct yetty_ymsdf_gen *self, uint32_t codepoint)
{
	auto *gen = reinterpret_cast<struct yetty_ymsdf_gen_cpu *>(self);

	struct yetty_ymsdf_gen_glyph glyph;
	memset(&glyph, 0, sizeof(glyph));
	glyph.codepoint = codepoint;

	msdfgen::Shape shape;
	double advance;
	if (!msdfgen::loadGlyph(shape, gen->font, codepoint, &advance))
		return YETTY_ERR(yetty_ymsdf_gen_glyph, "loadGlyph failed");

	glyph.advance = (float)(advance * gen->font_scale);

	/* Empty glyph (space) */
	if (shape.contours.empty()) {
		glyph.width = 0;
		glyph.height = 0;
		glyph.pixels = NULL;
		return YETTY_OK(yetty_ymsdf_gen_glyph, glyph);
	}

	shape.normalize();
	shape.orientContours();

	msdfgen::Shape::Bounds bounds = shape.getBounds();

	int padding = (int)ceil(gen->pixel_range);
	double logical_w = (bounds.r - bounds.l) * gen->font_scale;
	double logical_h = (bounds.t - bounds.b) * gen->font_scale;
	int bmp_w = (int)ceil(logical_w) + padding * 2;
	int bmp_h = (int)ceil(logical_h) + padding * 2;

	if (bmp_w <= 0 || bmp_h <= 0) {
		glyph.width = 0;
		glyph.height = 0;
		glyph.pixels = NULL;
		return YETTY_OK(yetty_ymsdf_gen_glyph, glyph);
	}

	glyph.width = (uint16_t)bmp_w;
	glyph.height = (uint16_t)bmp_h;
	glyph.bearing_x = (float)(bounds.l * gen->font_scale - padding);
	glyph.bearing_y = (float)(bounds.t * gen->font_scale + padding);

	msdfgen::edgeColoringSimple(shape, 3.0);

	msdfgen::Bitmap<float, 3> msdf(bmp_w, bmp_h);
	msdfgen::Vector2 translate(
		padding / gen->font_scale - bounds.l,
		padding / gen->font_scale - bounds.b);
	msdfgen::generateMSDF(msdf, shape, gen->pixel_range,
			      gen->font_scale, translate);

	/* Convert to RGBA8 with Y-flip */
	size_t pixel_bytes = (size_t)bmp_w * bmp_h * 4;
	glyph.pixels = (uint8_t *)malloc(pixel_bytes);
	if (!glyph.pixels)
		return YETTY_ERR(yetty_ymsdf_gen_glyph, "allocation failed");

	for (int y = 0; y < bmp_h; y++) {
		int src_y = bmp_h - 1 - y;
		for (int x = 0; x < bmp_w; x++) {
			size_t idx = ((size_t)y * bmp_w + x) * 4;
			glyph.pixels[idx + 0] = (uint8_t)std::clamp(
				msdf(x, src_y)[0] * 255.0f, 0.0f, 255.0f);
			glyph.pixels[idx + 1] = (uint8_t)std::clamp(
				msdf(x, src_y)[1] * 255.0f, 0.0f, 255.0f);
			glyph.pixels[idx + 2] = (uint8_t)std::clamp(
				msdf(x, src_y)[2] * 255.0f, 0.0f, 255.0f);
			glyph.pixels[idx + 3] = 255;
		}
	}

	return YETTY_OK(yetty_ymsdf_gen_glyph, glyph);
}

static const struct yetty_ymsdf_gen_ops cpu_ops = {
	cpu_destroy,
	cpu_get_metrics,
	cpu_generate_glyph,
};

extern "C" struct yetty_ymsdf_gen_result
yetty_ymsdf_gen_cpu_create(const char *ttf_path,
			   float font_size,
			   float pixel_range)
{
	if (!ttf_path)
		return YETTY_ERR(yetty_ymsdf_gen, "ttf_path is NULL");

	auto *gen = (struct yetty_ymsdf_gen_cpu *)calloc(
		1, sizeof(struct yetty_ymsdf_gen_cpu));
	if (!gen)
		return YETTY_ERR(yetty_ymsdf_gen, "allocation failed");

	gen->base.ops = &cpu_ops;
	gen->font_size = font_size > 0 ? font_size : 32.0f;
	gen->pixel_range = pixel_range > 0 ? pixel_range : 4.0f;

	gen->ft = msdfgen::initializeFreetype();
	if (!gen->ft) {
		free(gen);
		return YETTY_ERR(yetty_ymsdf_gen, "FreeType init failed");
	}

	gen->font = msdfgen::loadFont(gen->ft, ttf_path);
	if (!gen->font) {
		msdfgen::deinitializeFreetype(gen->ft);
		free(gen);
		return YETTY_ERR(yetty_ymsdf_gen, "failed to load font");
	}

	msdfgen::getFontMetrics(gen->metrics, gen->font);
	double em = gen->metrics.emSize > 0 ? gen->metrics.emSize
		    : (gen->metrics.ascenderY - gen->metrics.descenderY);
	gen->font_scale = gen->font_size / em;

	return YETTY_OK(yetty_ymsdf_gen, &gen->base);
}
