/*
 * osc.c - OSC 666674 envelope writer.
 *
 * Wire format (consumed by yterm/ypaint-layer.c):
 *   ESC ] 666674 ; --bin ; <base64(LZ4F(framed payload))> ESC \
 *
 * The framed payload is the magic-tagged blob produced by
 * yetty_ypaint_core_buffer_serialize() — prims + text_spans + scene_bounds.
 * yetty_yface owns the LZ4F + base64 + envelope construction.
 */

#include <yetty/ycat/ycat.h>

#include <yetty/yface/yface.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ycore/types.h>

#include <stdio.h>
#include <stdlib.h>

#define YCAT_OSC_VENDOR_ID 666674

size_t yetty_ycat_osc_bin_emit(const struct yetty_ypaint_core_buffer *buffer,
			       FILE *out)
{
	if (!buffer || !out)
		return 0;

	const uint8_t *raw_bytes = NULL;
	size_t raw_size = yetty_ypaint_core_buffer_serialize(
		(struct yetty_ypaint_core_buffer *)buffer, &raw_bytes);
	if (raw_size == 0 || !raw_bytes)
		return 0;

	struct yetty_ycore_buffer envelope = {0};
	struct yetty_ycore_void_result r = yetty_yface_emit(
		YCAT_OSC_VENDOR_ID, "--bin", raw_bytes, raw_size, &envelope);
	if (YETTY_IS_ERR(r)) {
		yetty_ycore_buffer_destroy(&envelope);
		return 0;
	}

	size_t written = 0;
	if (envelope.size > 0)
		written = fwrite(envelope.data, 1, envelope.size, out);
	yetty_ycore_buffer_destroy(&envelope);
	return written;
}
