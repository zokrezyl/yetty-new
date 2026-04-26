/*
 * yetty/yface/yface.h — bidirectional OSC stream wrapper.
 *
 * yface owns two growable byte buffers and the codec state. The OSC code
 * itself identifies the message type; there are no verbs in the body.
 *
 *   in_buf   — accumulates DECODED payload bytes (after b64 decode
 *              + optional LZ4F decompress) for a single received envelope.
 *              Cleared at start of each envelope.
 *   out_buf  — accumulates the OSC ENVELOPE for transport
 *              (\e]<code>;<compflag>;<base64[(LZ4F)payload]>\e\\). The
 *              caller ships out_buf.data[..size] over its transport (PTY,
 *              socket, ymux pipe) and clears it afterwards.
 *
 * The single character right after the first ';' is the compression flag:
 *   '0' — raw b64 of the payload (short events: mouse, resize, …)
 *   '1' — LZ4F-compressed then b64 (large payloads: frames, textures, video)
 *
 * Outgoing pipeline (caller → wire):
 *
 *     yetty_yface_start_write(yface, osc_code, compressed);
 *         emits "\e]<code>;<flag>;" raw, opens an LZ4F frame if requested,
 *         starts the streaming b64 encoder.
 *
 *     yetty_yface_write(yface, src, len);          // any number of times
 *         feeds bytes into the codec; produced bytes go through b64 into
 *         out_buf. May emit zero bytes (LZ4F batches up to its block size).
 *
 *     yetty_yface_finish_write(yface);
 *         flushes codec + b64 + writes "\e\\". out_buf is now a complete
 *         OSC sequence ready for write(2).
 *
 * Incoming pipeline (wire → consumer):
 *
 *     yetty_yface_set_handlers(yface, on_osc, on_raw, user);
 *     yetty_yface_feed_bytes(yface, raw, n);         // any number of times
 *
 *         The stream scanner finds \e]…\e\\ envelopes in `raw`, decodes
 *         their bodies (b64 ± LZ4F per the compflag) into in_buf, then
 *         fires `on_osc(user, osc_code, in_buf.data, in_buf.size)`.
 *         Any bytes outside an envelope are forwarded verbatim through
 *         `on_raw` so consumers can keep their raw-byte handling
 *         (keyboard, CSI, …) alongside structured OSC events.
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

/* Opaque — codec + scanner state lives in the .c file. */
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
 *
 * `compressed`:
 *   0 — raw b64 only (no LZ4). Use for short structured payloads where
 *       LZ4 framing would dominate (mouse events, resize, …).
 *   1 — LZ4F frame, then b64. Use for large payloads where the wire
 *       savings outweigh the framing overhead (ImGui frames, textures,
 *       video, multi-MB blobs).
 *---------------------------------------------------------------------------*/
/* `prefix` (may be NULL) is written raw between the compflag and the
 * b64 body, terminated by ';'. Lets verb-style emitters (ypaint
 * `--bin` / `--clear`, …) keep working alongside code-only dispatch.
 * Pass NULL when no verb is needed (the new ymgui input/output codes). */
struct yetty_ycore_void_result yetty_yface_start_write (struct yetty_yface *yface,
                                                        int osc_code,
                                                        int compressed,
                                                        const char *prefix);
struct yetty_ycore_void_result yetty_yface_write       (struct yetty_yface *yface,
                                                        const void *src, size_t len);
struct yetty_ycore_void_result yetty_yface_finish_write(struct yetty_yface *yface);

/*-----------------------------------------------------------------------------
 * Incoming — stream-scanner mode
 *
 * Caller pushes raw bytes (e.g. read(stdin) output). yface scans for
 * \e]…\e\\ envelopes, decodes the body, and emits one on_osc callback per
 * complete envelope. Anything outside an envelope is forwarded byte-for-
 * byte through on_raw — that's where the consumer plugs their keyboard /
 * CSI parser.
 *
 * Either callback may be NULL to discard that direction.
 *---------------------------------------------------------------------------*/
typedef void (*yetty_yface_msg_cb)(void *user, int osc_code,
                                   const uint8_t *payload, size_t len);
typedef void (*yetty_yface_raw_cb)(void *user, const char *bytes, size_t n);

void yetty_yface_set_handlers(struct yetty_yface *yface,
                              yetty_yface_msg_cb on_osc,
                              yetty_yface_raw_cb on_raw,
                              void *user);

struct yetty_ycore_void_result yetty_yface_feed_bytes(struct yetty_yface *yface,
                                                      const char *bytes, size_t n);

/*-----------------------------------------------------------------------------
 * Low-level read API — for callers that already hold the body
 *
 * The stream API above is the primary read path; this remains for
 * call sites that have the b64 body extracted by some other parser
 * (e.g. yterm/pty-reader-driven layer dispatch). `compressed` must
 * match what the writer used.
 *---------------------------------------------------------------------------*/
struct yetty_ycore_void_result yetty_yface_start_read  (struct yetty_yface *yface,
                                                        int compressed);
struct yetty_ycore_void_result yetty_yface_feed        (struct yetty_yface *yface,
                                                        const char *b64, size_t n);
struct yetty_ycore_void_result yetty_yface_finish_read (struct yetty_yface *yface);

/*-----------------------------------------------------------------------------
 * One-shot helpers for callers that emit / consume a single OSC at a time
 * (ygui, ycat, ymarkdown, yrich, …). They internally spin up a transient
 * yface, do the encode/decode, tear it down. For high-rate emitters that
 * stream many OSCs back-to-back (the ymgui RenderDrawData path) keep a
 * long-lived yetty_yface around instead — saves the LZ4 context alloc.
 *---------------------------------------------------------------------------*/

/* Encode `body` and append the full OSC sequence
 *   "\e]<osc_code>;<flag>;[<prefix>;]<base64[(LZ4F)body]>\e\\"
 * to `out_buf`. `prefix` may be NULL/empty (no verb — the modern
 * code-only path). `body` may be NULL/0 (envelope around an empty
 * payload — useful for `--clear`-style commands). `compressed` matches
 * yetty_yface_start_write. */
struct yetty_ycore_void_result yetty_yface_emit(
    int osc_code, int compressed, const char *prefix,
    const void *body, size_t body_len,
    struct yetty_ycore_buffer *out_buf);

/* Same, but write the full envelope straight to `fd` (blocking write).
 * Convenience for low-rate emitters that don't have their own queueing. */
struct yetty_ycore_void_result yetty_yface_emit_to_fd(
    int fd, int osc_code, int compressed, const char *prefix,
    const void *body, size_t body_len);

/* Decode an OSC body (the bytes the caller has isolated as the b64
 * payload between `<flag>;[<prefix>;]` and the trailing ESC\) into
 * `out_buf`. `compressed` must match what the writer used. */
struct yetty_ycore_void_result yetty_yface_decode(
    const char *b64, size_t n, int compressed,
    struct yetty_ycore_buffer *out_buf);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YFACE_YFACE_H */
