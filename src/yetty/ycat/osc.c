/*
 * osc.c - base64 encoder + OSC 666674 envelope writer.
 *
 * The OSC format consumed by yterm/ypaint-layer.c is
 *   ESC ] 666674 ; <args> ; <payload> ESC \
 * where <args> here is just "--bin" and <payload> is the base64 of the
 * ypaint-core buffer's primitives bytes (what create_from_base64 expects).
 * Streaming encode (no intermediate allocation) keeps large PDFs cheap.
 */

#include <yetty/ycat/ycat.h>

#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/types.h>

#include <stdio.h>

/* OSC 666674 — ypaint scrolling layer sink (see include/yetty/yterm/pty-reader.h). */
#define YCAT_OSC_VENDOR_ID 666674

static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Streaming base64 encoder: emits 4 chars per 3 input bytes, plus terminal
 * padding. Writes directly to `out`. Returns 0 on success, -1 on short write. */
static int base64_emit(const uint8_t *data, size_t len, FILE *out)
{
	char buf[4];
	size_t i = 0;

	while (i + 2 < len) {
		uint32_t triple = ((uint32_t)data[i]     << 16) |
				  ((uint32_t)data[i + 1] <<  8) |
				   (uint32_t)data[i + 2];
		buf[0] = b64[(triple >> 18) & 0x3F];
		buf[1] = b64[(triple >> 12) & 0x3F];
		buf[2] = b64[(triple >>  6) & 0x3F];
		buf[3] = b64[ triple        & 0x3F];
		if (fwrite(buf, 1, 4, out) != 4)
			return -1;
		i += 3;
	}

	if (i + 1 == len) {
		uint32_t v = (uint32_t)data[i] << 16;
		buf[0] = b64[(v >> 18) & 0x3F];
		buf[1] = b64[(v >> 12) & 0x3F];
		buf[2] = '=';
		buf[3] = '=';
		if (fwrite(buf, 1, 4, out) != 4)
			return -1;
	} else if (i + 2 == len) {
		uint32_t v = ((uint32_t)data[i]     << 16) |
			     ((uint32_t)data[i + 1] <<  8);
		buf[0] = b64[(v >> 18) & 0x3F];
		buf[1] = b64[(v >> 12) & 0x3F];
		buf[2] = b64[(v >>  6) & 0x3F];
		buf[3] = '=';
		if (fwrite(buf, 1, 4, out) != 4)
			return -1;
	}
	return 0;
}

size_t yetty_ycat_osc_bin_emit(const struct yetty_ypaint_core_buffer *buffer,
			       FILE *out)
{
	if (!buffer || !out)
		return 0;

	const struct yetty_ycore_buffer *prims =
		yetty_ypaint_core_buffer_primitives(buffer);
	if (!prims)
		return 0;

	/* ESC ] vendor_id ; args ; <base64> ESC \ */
	int header = fprintf(out, "\x1b]%d;--bin;", YCAT_OSC_VENDOR_ID);
	if (header < 0)
		return 0;

	if (base64_emit(prims->data, prims->size, out) < 0)
		return 0;

	if (fwrite("\x1b\\", 1, 2, out) != 2)
		return 0;

	/* Return a non-zero byte count (we don't track exact size through the
	 * streaming path; callers only check != 0 for "success"). */
	return (size_t)header + 2;
}
