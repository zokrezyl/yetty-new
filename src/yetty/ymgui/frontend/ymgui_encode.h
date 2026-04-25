/*
 * ymgui_encode.h — C helpers used by the frontend (client-side).
 * No ImGui dependency. Safe to compile as C99.
 *
 *   ymgui_buf_*   — growable byte buffer for building the wire payload
 *   ymgui_osc_*   — wrap a buffer with OSC envelope + base64 and write(2) it
 *   ymgui_b64_*   — base64 encoder (OSC payloads are base64 per ygui precedent)
 */

#ifndef YETTY_YMGUI_FRONTEND_ENCODE_H
#define YETTY_YMGUI_FRONTEND_ENCODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Growable byte buffer.
 *-------------------------------------------------------------------------*/
struct ymgui_buf {
    uint8_t *data;
    size_t   size;
    size_t   cap;
};

void  ymgui_buf_init(struct ymgui_buf *b);
void  ymgui_buf_free(struct ymgui_buf *b);
void  ymgui_buf_reset(struct ymgui_buf *b);
int   ymgui_buf_reserve(struct ymgui_buf *b, size_t extra); /* 0 on success */
int   ymgui_buf_write(struct ymgui_buf *b, const void *src, size_t n);

/* Reserve `n` bytes, return pointer to the reserved region (advances size).
 * Returns NULL on allocation failure. Useful when the caller wants to fill
 * a struct in-place (avoids an extra memcpy). */
void *ymgui_buf_alloc(struct ymgui_buf *b, size_t n);

/* Pad `b->size` up to a multiple of `align`. */
int   ymgui_buf_align(struct ymgui_buf *b, size_t align);

/*---------------------------------------------------------------------------
 * Base64 encode. `out` must have room for 4*((size+2)/3) bytes (no NUL).
 * Returns the number of bytes written.
 *-------------------------------------------------------------------------*/
size_t ymgui_b64_encoded_len(size_t raw_size);
size_t ymgui_b64_encode(const uint8_t *src, size_t size, char *out);

/*---------------------------------------------------------------------------
 * OSC transport. Non-blocking: tries to write the full envelope
 * `\e]<vendor>;<verb>[;<base64(payload)>]\e\\` to `fd`. Caller is
 * expected to have set `fd` to O_NONBLOCK already.
 *
 * Return values:
 *    0  full message accepted (either flushed straight to fd, or queued
 *       in a small in-flight buffer for later draining).
 *    1  message dropped because a previous OSC's tail is still pending
 *       — the wire would be corrupted by two interleaved OSC bodies.
 *       Callers should treat this as "frame skipped, ImGui re-renders
 *       next iter".
 *   -1  hard error (write returned non-EAGAIN errno).
 *
 * The drain happens opportunistically: each call first tries to push
 * any leftover bytes from a previous call. To wake on stdout-ready,
 * pair with `ymgui_osc_pending()` + poll(POLLOUT) externally.
 *-------------------------------------------------------------------------*/
int ymgui_osc_write(int fd, const char *verb,
                    const uint8_t *payload, size_t payload_size);

/* Try to push any queued bytes from a previous ymgui_osc_write. Used by
 * the platform's WaitInput when stdout becomes write-ready, to drain the
 * tail without needing a fresh emit to trigger it. Returns the same
 * tri-state as ymgui_osc_write (0 = drained or nothing to do, 1 =
 * still pending, -1 = error). */
int ymgui_osc_flush(int fd);

/* True if a previous osc_write left bytes queued for the next flush. */
int ymgui_osc_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMGUI_FRONTEND_ENCODE_H */
