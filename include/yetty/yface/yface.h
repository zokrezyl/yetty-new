/*
 * yetty/yface/yface.h — streaming OSC stream wrapper.
 *
 * yetty_yface owns two growable byte buffers:
 *   in_buf   — accumulates DECODED payload bytes (after b64 decode +
 *              LZ4F decompress) for the consumer to read.
 *   out_buf  — accumulates the OSC ENVELOPE for transport
 *              (\e]<code>;<base64(LZ4F(payload))>\e\\). The caller
 *              ships out_buf.data[..size] over its transport (PTY,
 *              socket, ymux pipe) and clears it afterwards.
 *
 * Outgoing pipeline (caller → wire):
 *
 *     yetty_yface_start_write(yface, osc_code);
 *         emits "\e]<code>;" raw, opens an LZ4F frame, runs the frame
 *         header through the streaming base64 encoder into out_buf.
 *
 *     yetty_yface_write(yface, src, len);          // any number of times
 *         feeds bytes into LZ4F; whatever LZ4F emits goes through the
 *         streaming b64 encoder into out_buf. May emit zero bytes — LZ4F
 *         batches up to its block size internally.
 *
 *     yetty_yface_finish_write(yface);
 *         flushes LZ4F (frame footer + buffered bytes), pads + writes
 *         any remaining b64 chars + "\e\\". Out_buf is now a complete
 *         OSC sequence ready for write(2).
 *
 * Incoming pipeline (wire → consumer):
 *
 *     yetty_yface_start_read(yface);
 *         resets the streaming b64 + LZ4F decode state. in_buf is
 *         cleared.
 *
 *     yetty_yface_feed(yface, b64_chars, n);       // any number of times
 *         decodes the chars (carry partial), feeds the bytes into
 *         LZ4F_decompress, appends the decompressed output to in_buf.
 *
 *     yetty_yface_finish_read(yface);
 *         finalizes the LZ4F context. in_buf is the fully decoded
 *         payload from <yface_start_write..yface_finish_write> on the
 *         peer side.
 *
 * Compression is always on (LZ4 frame). Cost is dominated by base64
 * everywhere — at lz4 level 1 the compressor adds <2 % CPU and saves
 * 60–80 % of the wire bytes for ImGui-style geometry.
 */

#ifndef YETTY_YFACE_YFACE_H
#define YETTY_YFACE_YFACE_H

#include <stddef.h>
#include <stdint.h>

#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque — compressor + base64 state lives in the .c file. */
struct yetty_yface;

YETTY_YRESULT_DECLARE(yetty_yface_ptr, struct yetty_yface *);

/*-----------------------------------------------------------------------------
 * Lifecycle
 *---------------------------------------------------------------------------*/
struct yetty_yface_ptr_result yetty_yface_create(void);
void                          yetty_yface_destroy(struct yetty_yface *yface);

/*-----------------------------------------------------------------------------
 * Buffer access — in_buf / out_buf are owned by yface; caller may read
 * them and clear them via yetty_ycore_buffer_clear() when transferred.
 * The pointers are stable for the lifetime of the yface.
 *---------------------------------------------------------------------------*/
struct yetty_ycore_buffer *yetty_yface_in_buf (struct yetty_yface *yface);
struct yetty_ycore_buffer *yetty_yface_out_buf(struct yetty_yface *yface);

/*-----------------------------------------------------------------------------
 * Outgoing — appends to out_buf
 *---------------------------------------------------------------------------*/
/* `prefix` is written raw, followed by ';', between the OSC code and the
 * compressed/base64'd body. Lets callers attach a verb (e.g. "--frame")
 * to the OSC body so existing argument parsers see the same shape they
 * always have. NULL = no prefix; just "\e]<code>;<b64>...\e\\". */
struct yetty_ycore_void_result yetty_yface_start_write (struct yetty_yface *yface,
                                                        int osc_code,
                                                        const char *prefix);
struct yetty_ycore_void_result yetty_yface_write       (struct yetty_yface *yface,
                                                        const void *src, size_t len);
struct yetty_ycore_void_result yetty_yface_finish_write(struct yetty_yface *yface);

/*-----------------------------------------------------------------------------
 * Incoming — appends to in_buf
 *---------------------------------------------------------------------------*/
struct yetty_ycore_void_result yetty_yface_start_read  (struct yetty_yface *yface);
struct yetty_ycore_void_result yetty_yface_feed        (struct yetty_yface *yface,
                                                        const char *b64, size_t n);
struct yetty_ycore_void_result yetty_yface_finish_read (struct yetty_yface *yface);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YFACE_YFACE_H */
