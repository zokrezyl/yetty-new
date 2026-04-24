/*
 * osc.c - OSC 666674 envelope writer.
 *
 * Format consumed by yterm/ypaint-layer.c is
 *   ESC ] 666674 ; --bin ; <base64> ESC \
 * where <base64> is yetty_ypaint_core_buffer_to_base64() of the buffer —
 * i.e. the framed wire payload (magic + scene_bounds + prims + text_spans)
 * already base64-encoded in a single pass. The receiver's
 * create_from_base64 detects the magic and restores every section,
 * including text spans, so coloured text from tree-sitter / ymarkdown /
 * ypdf actually crosses the wire.
 */

#include <yetty/ycat/ycat.h>

#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/types.h>

#include <stdio.h>
#include <stdlib.h>

/* OSC 666674 — ypaint scrolling layer sink (see include/yetty/yterm/pty-reader.h). */
#define YCAT_OSC_VENDOR_ID 666674

size_t yetty_ycat_osc_bin_emit(const struct yetty_ypaint_core_buffer *buffer,
			       FILE *out)
{
	if (!buffer || !out)
		return 0;

	struct yetty_ycore_buffer_result br =
		yetty_ypaint_core_buffer_to_base64(buffer);
	if (YETTY_IS_ERR(br))
		return 0;

	int header = fprintf(out, "\x1b]%d;--bin;", YCAT_OSC_VENDOR_ID);
	if (header < 0) {
		free(br.value.data);
		return 0;
	}

	size_t payload = br.value.size;
	if (payload > 0 &&
	    fwrite(br.value.data, 1, payload, out) != payload) {
		free(br.value.data);
		return 0;
	}

	if (fwrite("\x1b\\", 1, 2, out) != 2) {
		free(br.value.data);
		return 0;
	}

	free(br.value.data);
	return (size_t)header + payload + 2;
}
