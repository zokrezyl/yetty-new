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
 * OSC transport. Writes `\e]<vendor>;<verb>[;<base64(payload)>]\e\\` to the
 * given fd. Returns 0 on success, -1 on error.
 *
 * `payload`/`payload_size` may be NULL/0 for verbs without a body (--clear).
 *-------------------------------------------------------------------------*/
int ymgui_osc_write(int fd, const char *verb,
                    const uint8_t *payload, size_t payload_size);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMGUI_FRONTEND_ENCODE_H */
