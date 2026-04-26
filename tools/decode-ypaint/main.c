/*
 * decode-ypaint — diagnostic tool for the OSC stream emitted by ycat (or any
 * other ypaint-bin emitter). Walks a captured byte file looking for
 * `ESC ] <code> ; <args> ; <b64-payload> ESC \` envelopes, runs them through
 * the SAME yface + ypaint-core decode path the ypaint-layer uses on the
 * receiving side, and prints what it found.
 *
 * Usage:
 *   ycat input.pdf > /tmp/cap.bin
 *   decode-ypaint /tmp/cap.bin
 *
 * For each envelope it prints: OSC code, args (e.g. "--bin"), b64 payload
 * size, decompressed size, ypaint-core magic check, and primitive count if
 * the buffer parses. A clean run means the wire is fine and any visible
 * mis-render is in the receiver (canvas / shader / GPU upload).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/ycore/util.h>
#include <yetty/yface/yface.h>
#include <yetty/yterm/osc-args.h>
#include <yetty/ypaint/flyweight.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ypaint-core/font-prim.h>
#include <yetty/ypaint-core/text-span-prim.h>

/* Avoid pulling complex-prim-types.h (it transitively includes webgpu.h);
 * we only need the type-id base constant for classification stats. */
#define YETTY_YPAINT_COMPLEX_TYPE_BASE 0x80000000u

/* CLI: -v / --verbose dumps per-prim details (position, font, text snippet).
 * -h / --histogram prints a y-bucket histogram of text-span placement so we
 * can spot spans that all collapse onto a few rows or escape the canvas. */
static int g_verbose = 0;
static int g_histogram = 0;

/* Decode one envelope body — i.e. the bytes BETWEEN `ESC ]` and `ESC \`.
 * Shape: "<code>;<args...>;<b64-payload>". */
static int decode_envelope(struct yetty_yface *y,
                           struct yetty_ypaint_flyweight_registry *fw,
                           const char *body, size_t body_len)
{
	/* Vendor id up to first ';'. */
	const char *semi1 = memchr(body, ';', body_len);
	if (!semi1) {
		fprintf(stderr, "  ERR: no ';' after OSC code\n");
		return -1;
	}
	size_t code_len = semi1 - body;
	if (code_len == 0 || code_len > 12) {
		fprintf(stderr, "  ERR: bad OSC code length %zu\n", code_len);
		return -1;
	}
	char code_str[16] = {0};
	memcpy(code_str, body, code_len);
	int osc_code = atoi(code_str);

	/* Reuse the same parser the ypaint-layer uses, on `<args>;<b64>`. */
	const char *post_code = semi1 + 1;
	size_t post_code_len = body_len - code_len - 1;
	struct yetty_yterm_osc_args args = {0};
	if (yetty_yterm_osc_args_parse(&args, post_code, post_code_len) < 0) {
		fprintf(stderr, "  ERR: osc_args_parse failed\n");
		return -1;
	}

	int has_bin = yetty_yterm_osc_args_has(&args, "bin");
	int has_yaml = yetty_yterm_osc_args_has(&args, "yaml");
	int has_clear = yetty_yterm_osc_args_has(&args, "clear");

	fprintf(stderr,
		"  osc=%d args=[%s%s%s] payload_b64=%zu\n",
		osc_code,
		has_bin ? "--bin " : "",
		has_yaml ? "--yaml " : "",
		has_clear ? "--clear " : "",
		args.payload_len);

	if (has_clear) {
		yetty_yterm_osc_args_free(&args);
		return 0;
	}
	if (!has_bin) {
		fprintf(stderr, "  (skipping non-bin envelope)\n");
		yetty_yterm_osc_args_free(&args);
		return 0;
	}
	if (args.payload_len == 0) {
		fprintf(stderr, "  payload empty\n");
		yetty_yterm_osc_args_free(&args);
		return 0;
	}

	/* Same decode the layer does: get_payload_buffer → b64 → LZ4F. */
	struct yetty_ycore_buffer_result pl =
		yetty_yterm_osc_args_get_payload_buffer(&args);
	if (!pl.ok) {
		fprintf(stderr, "  ERR: get_payload_buffer: %s\n", pl.error.msg);
		yetty_yterm_osc_args_free(&args);
		return -1;
	}

	struct yetty_ycore_void_result r = yetty_yface_start_read(y);
	if (!r.ok) {
		fprintf(stderr, "  ERR: start_read: %s\n", r.error.msg);
		yetty_yterm_osc_args_free(&args);
		return -1;
	}
	r = yetty_yface_feed(y, (const char *)pl.value.data, pl.value.size);
	if (!r.ok) {
		fprintf(stderr, "  ERR: feed: %s\n", r.error.msg);
		yetty_yface_finish_read(y);
		yetty_yterm_osc_args_free(&args);
		return -1;
	}
	r = yetty_yface_finish_read(y);
	if (!r.ok) {
		fprintf(stderr, "  ERR: finish_read: %s\n", r.error.msg);
		yetty_yterm_osc_args_free(&args);
		return -1;
	}

	struct yetty_ycore_buffer *in = yetty_yface_in_buf(y);
	fprintf(stderr,
		"  decompressed=%zu bytes; first 16:",
		in->size);
	for (size_t i = 0; i < in->size && i < 16; i++)
		fprintf(stderr, " %02x", (unsigned char)in->data[i]);
	fprintf(stderr, "\n");

	/* Try the same buffer_create_from_bytes the ypaint-layer uses. If it
	 * succeeds, walk the prims via the flyweight registry to confirm every
	 * primitive parses. This is the receiver's exact code path — mirroring
	 * it here separates wire bugs from canvas/shader bugs cleanly. */
	struct yetty_ypaint_core_buffer_result br =
		yetty_ypaint_core_buffer_create_from_bytes(in->data, in->size);
	if (!br.ok) {
		fprintf(stderr, "  ERR: buffer_create_from_bytes: %s\n",
			br.error.msg);
		yetty_yterm_osc_args_free(&args);
		return -1;
	}

	uint32_t prim_count = 0;
	uint64_t total_words = 0;
	uint64_t font_count = 0, text_span_count = 0, sdf_count = 0,
		 complex_count = 0;
	float min_x = 1e30f, max_x = -1e30f;
	float min_y = 1e30f, max_y = -1e30f;
	float min_fs = 1e30f, max_fs = -1e30f;
	int min_fid = 1 << 30, max_fid = -(1 << 30);
	uint32_t total_text_chars = 0;

	/* y-bucket histogram of text-span starts, in 50-pixel buckets. We pick
	 * 64 buckets and let the y range collapse / saturate; helps spot the
	 * "all spans land at y=0" case versus "spans cover whole document". */
	enum { HBUCKETS = 64 };
	uint32_t hist[HBUCKETS] = {0};

	struct yetty_ypaint_core_primitive_iter_result it =
		yetty_ypaint_core_buffer_prim_first(br.value, fw);
	if (it.ok) {
		struct yetty_ypaint_core_primitive_iter cur = it.value;
		while (1) {
			prim_count++;
			uint32_t type = cur.fw.data[0];
			if (type == YETTY_YPAINT_TYPE_FONT) {
				font_count++;
			} else if (type == YETTY_YPAINT_TYPE_TEXT_SPAN) {
				text_span_count++;
				struct yetty_ypaint_text_span_prim_view v;
				if (yetty_ypaint_text_span_prim_parse(
					    cur.fw.data, &v) == 0) {
					if (v.x < min_x) min_x = v.x;
					if (v.x > max_x) max_x = v.x;
					if (v.y < min_y) min_y = v.y;
					if (v.y > max_y) max_y = v.y;
					if (v.font_size < min_fs)
						min_fs = v.font_size;
					if (v.font_size > max_fs)
						max_fs = v.font_size;
					if (v.font_id < min_fid)
						min_fid = v.font_id;
					if (v.font_id > max_fid)
						max_fid = v.font_id;
					total_text_chars += v.text_len;
					if (g_verbose) {
						int len = (int)v.text_len;
						if (len > 40) len = 40;
						fprintf(stderr,
							"    span x=%8.2f y=%9.2f size=%5.2f fid=%2d color=0x%08x len=%4u text=\"%.*s\"\n",
							v.x, v.y, v.font_size,
							v.font_id, v.color,
							v.text_len, len, v.text);
					}
				}
			} else if (type >= YETTY_YPAINT_COMPLEX_TYPE_BASE) {
				complex_count++;
			} else {
				sdf_count++;
			}

			if (cur.fw.ops && cur.fw.ops->size) {
				struct yetty_ycore_size_result sz =
					cur.fw.ops->size(cur.fw.data);
				if (sz.ok)
					total_words +=
						(uint64_t)sz.value /
						sizeof(uint32_t);
			}

			struct yetty_ypaint_core_primitive_iter_result nxt =
				yetty_ypaint_core_buffer_prim_next(br.value,
								   fw, &cur);
			if (!nxt.ok)
				break;
			cur = nxt.value;
		}
	}

	fprintf(stderr,
		"  parsed: %u prims (font=%llu text_span=%llu sdf=%llu "
		"complex=%llu) total_words=%llu\n",
		prim_count, (unsigned long long)font_count,
		(unsigned long long)text_span_count,
		(unsigned long long)sdf_count,
		(unsigned long long)complex_count,
		(unsigned long long)total_words);

	if (text_span_count > 0) {
		fprintf(stderr,
			"  text spans: x=[%.2f..%.2f] y=[%.2f..%.2f] "
			"font_size=[%.2f..%.2f] font_id=[%d..%d] "
			"total_chars=%u\n",
			min_x, max_x, min_y, max_y, min_fs, max_fs,
			min_fid, max_fid, total_text_chars);

		/* Bucket the y of each span into HBUCKETS equal slices of
		 * [min_y, max_y]. Re-iterate (cheap; in_buf is small). */
		float yspan = max_y - min_y;
		if (yspan > 0 && g_histogram) {
			it = yetty_ypaint_core_buffer_prim_first(br.value, fw);
			if (it.ok) {
				struct yetty_ypaint_core_primitive_iter cur =
					it.value;
				while (1) {
					if (cur.fw.data[0] ==
					    YETTY_YPAINT_TYPE_TEXT_SPAN) {
						struct yetty_ypaint_text_span_prim_view v;
						if (yetty_ypaint_text_span_prim_parse(
							    cur.fw.data,
							    &v) == 0) {
							float t =
								(v.y - min_y) /
								yspan;
							int b = (int)(t *
								      (HBUCKETS - 1));
							if (b < 0) b = 0;
							if (b >= HBUCKETS)
								b = HBUCKETS - 1;
							hist[b]++;
						}
					}
					struct yetty_ypaint_core_primitive_iter_result nxt =
						yetty_ypaint_core_buffer_prim_next(
							br.value, fw, &cur);
					if (!nxt.ok) break;
					cur = nxt.value;
				}
				fprintf(stderr,
					"  y-histogram (%d buckets, %.0f..%.0f):\n",
					HBUCKETS, min_y, max_y);
				uint32_t hmax = 0;
				for (int i = 0; i < HBUCKETS; i++)
					if (hist[i] > hmax) hmax = hist[i];
				for (int i = 0; i < HBUCKETS; i++) {
					float y0 = min_y + (yspan * i) /
								  HBUCKETS;
					int bar = hmax ? (int)((hist[i] *
								40.0f) /
							       hmax) : 0;
					fprintf(stderr,
						"    [%2d] y=%9.0f n=%6u %.*s\n",
						i, y0, hist[i], bar,
						"########################################");
				}
			}
		}
	}

	yetty_ypaint_core_buffer_destroy(br.value);
	yetty_yterm_osc_args_free(&args);
	return 0;
}

static int run(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsz < 0) {
		fclose(f);
		return 1;
	}
	size_t n = (size_t)fsz;
	char *buf = malloc(n + 1);
	if (!buf) {
		fclose(f);
		return 1;
	}
	if (fread(buf, 1, n, f) != n) {
		fprintf(stderr, "short read\n");
		free(buf);
		fclose(f);
		return 1;
	}
	buf[n] = 0;
	fclose(f);

	struct yetty_yface_ptr_result yr = yetty_yface_create();
	if (!yr.ok) {
		fprintf(stderr, "yface_create failed: %s\n", yr.error.msg);
		free(buf);
		return 1;
	}
	struct yetty_yface *y = yr.value;

	/* Same flyweight registry the canvas builds — needed to walk prims. */
	struct yetty_ypaint_flyweight_registry_ptr_result fwr =
		yetty_ypaint_flyweight_create();
	if (!fwr.ok) {
		fprintf(stderr, "flyweight_create failed: %s\n",
			fwr.error.msg);
		yetty_yface_destroy(y);
		free(buf);
		return 1;
	}
	struct yetty_ypaint_flyweight_registry *fw = fwr.value;

	size_t pos = 0;
	int count = 0, errors = 0;
	while (pos + 1 < n) {
		if ((unsigned char)buf[pos] != 0x1B || buf[pos + 1] != ']') {
			pos++;
			continue;
		}
		size_t open = pos + 2;
		size_t close = open;
		while (close + 1 < n) {
			if ((unsigned char)buf[close] == 0x1B &&
			    buf[close + 1] == '\\')
				break;
			close++;
		}
		if (close + 1 >= n) {
			fprintf(stderr,
				"envelope #%d at byte %zu: unterminated "
				"(no ESC \\)\n",
				count, pos);
			errors++;
			break;
		}
		size_t body_len = close - open;
		fprintf(stderr,
			"envelope #%d at byte %zu (body %zu B):\n",
			count, pos, body_len);
		if (decode_envelope(y, fw, buf + open, body_len) < 0)
			errors++;
		count++;
		pos = close + 2;
	}

	fprintf(stderr, "\nfound %d envelope(s), %d error(s)\n", count,
		errors);

	yetty_ypaint_flyweight_registry_destroy(fw);
	yetty_yface_destroy(y);
	free(buf);
	return errors ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 ||
		    strcmp(argv[i], "--verbose") == 0)
			g_verbose = 1;
		else if (strcmp(argv[i], "-H") == 0 ||
			 strcmp(argv[i], "--histogram") == 0)
			g_histogram = 1;
		else if (argv[i][0] == '-') {
			fprintf(stderr,
				"usage: %s [-v|--verbose] [-H|--histogram] "
				"<ycat-output-file>\n",
				argv[0]);
			return 1;
		} else if (!path) {
			path = argv[i];
		}
	}
	if (!path) {
		fprintf(stderr,
			"usage: %s [-v|--verbose] [-H|--histogram] "
			"<ycat-output-file>\n",
			argv[0]);
		return 1;
	}
	return run(path);
}
