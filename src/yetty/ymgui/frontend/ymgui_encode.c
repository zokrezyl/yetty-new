/*
 * ymgui_encode.c — buffer, base64, OSC write. Pure C.
 */

#include "ymgui_encode.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define YMGUI_WRITE _write
#else
#include <unistd.h>
#define YMGUI_WRITE write
#endif

/*===========================================================================
 * Buffer
 *=========================================================================*/

void ymgui_buf_init(struct ymgui_buf *b)
{
    b->data = NULL;
    b->size = 0;
    b->cap  = 0;
}

void ymgui_buf_free(struct ymgui_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->size = 0;
    b->cap  = 0;
}

void ymgui_buf_reset(struct ymgui_buf *b)
{
    b->size = 0;
}

int ymgui_buf_reserve(struct ymgui_buf *b, size_t extra)
{
    size_t need = b->size + extra;
    if (need <= b->cap)
        return 0;

    size_t new_cap = b->cap ? b->cap * 2 : 1024;
    while (new_cap < need)
        new_cap *= 2;

    uint8_t *p = (uint8_t *)realloc(b->data, new_cap);
    if (!p)
        return -1;

    b->data = p;
    b->cap  = new_cap;
    return 0;
}

int ymgui_buf_write(struct ymgui_buf *b, const void *src, size_t n)
{
    if (ymgui_buf_reserve(b, n) != 0)
        return -1;

    memcpy(b->data + b->size, src, n);
    b->size += n;
    return 0;
}

void *ymgui_buf_alloc(struct ymgui_buf *b, size_t n)
{
    if (ymgui_buf_reserve(b, n) != 0)
        return NULL;

    void *p = b->data + b->size;
    b->size += n;
    return p;
}

int ymgui_buf_align(struct ymgui_buf *b, size_t align)
{
    size_t rem = b->size % align;
    if (rem == 0)
        return 0;

    size_t pad = align - rem;
    if (ymgui_buf_reserve(b, pad) != 0)
        return -1;

    memset(b->data + b->size, 0, pad);
    b->size += pad;
    return 0;
}

/*===========================================================================
 * Base64
 *=========================================================================*/

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ymgui_b64_encoded_len(size_t raw_size)
{
    return ((raw_size + 2) / 3) * 4;
}

size_t ymgui_b64_encode(const uint8_t *src, size_t size, char *out)
{
    size_t j = 0;

    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = ((uint32_t)src[i]) << 16;
        if (i + 1 < size)
            n |= ((uint32_t)src[i + 1]) << 8;
        if (i + 2 < size)
            n |= (uint32_t)src[i + 2];

        out[j++] = b64_table[(n >> 18) & 0x3F];
        out[j++] = b64_table[(n >> 12) & 0x3F];
        out[j++] = (i + 1 < size) ? b64_table[(n >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < size) ? b64_table[ n       & 0x3F] : '=';
    }
    return j;
}

/*===========================================================================
 * OSC write — non-blocking with single-message in-flight queue
 *
 * The model: at most ONE partially-sent OSC may be in flight at a time
 * (the tail of the previous message, holding however many bytes the
 * kernel refused with EAGAIN). Subsequent emits while that tail is
 * unflushed are dropped — interleaving a fresh OSC's bytes into the
 * middle of an unfinished one would corrupt yetty's parser. Callers
 * (the demo loop) treat a drop as "frame skipped, re-render next".
 *=========================================================================*/

#include <yetty/ymgui/wire.h>
#include <errno.h>

struct pending {
    uint8_t *data;     /* owned; freed when fully written */
    size_t   size;
    size_t   off;
};
static struct pending g_pending = { NULL, 0, 0 };

static int try_drain_pending(int fd)
{
    while (g_pending.off < g_pending.size) {
        ssize_t w = YMGUI_WRITE(fd,
                                (const char *)g_pending.data + g_pending.off,
                                (unsigned)(g_pending.size - g_pending.off));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1; /* still pending */
            return -1;
        }
        g_pending.off += (size_t)w;
    }
    free(g_pending.data);
    g_pending.data = NULL;
    g_pending.size = g_pending.off = 0;
    return 0;
}

int ymgui_osc_flush(int fd)
{
    if (!g_pending.data) return 0;
    return try_drain_pending(fd);
}

int ymgui_osc_pending(void)
{
    return g_pending.data != NULL;
}

int ymgui_osc_write(int fd, const char *verb,
                    const uint8_t *payload, size_t payload_size)
{
    /* If an earlier message is still draining, attempt one more push;
     * if it still won't fit, drop this new emit. We don't ever have
     * two concurrent partial messages — one corrupts the wire. */
    if (g_pending.data) {
        int r = try_drain_pending(fd);
        if (r < 0) return -1;
        if (r > 0) return 1; /* dropped */
    }

    /* Build: "\e]<vendor>;<verb>" [ ";<base64>" ] "\e\\"  */
    size_t verb_len = strlen(verb);
    size_t b64_len  = payload_size ? ymgui_b64_encoded_len(payload_size) : 0;
    size_t prefix_len = 2 + (sizeof(YMGUI_OSC_VENDOR) - 1) + 1 + verb_len;
    size_t middle_len = payload_size ? (1 + b64_len) : 0;
    size_t tail_len   = 2;
    size_t total = prefix_len + middle_len + tail_len;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    uint8_t *p = buf;
    *p++ = '\033';
    *p++ = ']';
    memcpy(p, YMGUI_OSC_VENDOR, sizeof(YMGUI_OSC_VENDOR) - 1);
    p += sizeof(YMGUI_OSC_VENDOR) - 1;
    *p++ = ';';
    memcpy(p, verb, verb_len);
    p += verb_len;
    if (payload_size) {
        *p++ = ';';
        size_t n = ymgui_b64_encode(payload, payload_size, (char *)p);
        p += n;
    }
    *p++ = '\033';
    *p++ = '\\';

    /* Try to write straight through. */
    size_t off = 0;
    while (off < total) {
        ssize_t w = YMGUI_WRITE(fd, (const char *)(buf + off),
                                (unsigned)(total - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Park the unsent tail. The malloc'd buffer becomes
                 * the queue — no extra copy. */
                if (off == 0) {
                    g_pending.data = buf;
                    g_pending.size = total;
                    g_pending.off  = 0;
                    return 0;
                }
                size_t left = total - off;
                uint8_t *tail = (uint8_t *)malloc(left);
                if (!tail) { free(buf); return -1; }
                memcpy(tail, buf + off, left);
                free(buf);
                g_pending.data = tail;
                g_pending.size = left;
                g_pending.off  = 0;
                return 0;
            }
            free(buf);
            return -1;
        }
        off += (size_t)w;
    }
    free(buf);
    return 0;
}
