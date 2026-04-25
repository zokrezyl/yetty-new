/*
 * yetty/yface/yface.c — streaming OSC stream:
 *   bytes  -->  LZ4F_compress*  -->  streaming base64 encode  -->  out_buf
 *   chars  -->  streaming base64 decode  -->  LZ4F_decompress  -->  in_buf
 *
 * The LZ4 contexts hold the dictionary / partial-block state; the b64
 * encoder/decoder hold the 0..2 / 0..3 byte carry that bridges chunked
 * input. Together that means start/write/finish can be called with
 * arbitrarily small chunks and produce identical output to a one-shot
 * encode of the concatenated input.
 */

#include <yetty/yface/yface.h>

#include <lz4frame.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*===========================================================================
 * Internal state
 *=========================================================================*/

#define ENC_SCRATCH_CAP_DEFAULT  (64 * 1024)   /* one LZ4 block worth */

struct yetty_yface {
    struct yetty_ycore_buffer in_buf;
    struct yetty_ycore_buffer out_buf;

    /* Outgoing — LZ4F + streaming b64 encode */
    LZ4F_compressionContext_t   enc_ctx;
    uint8_t                    *enc_scratch;
    size_t                      enc_scratch_cap;
    int                         enc_active;
    /* b64 carry: 0..2 input bytes that didn't form a complete triple yet. */
    uint8_t                     enc_b64_carry[2];
    uint8_t                     enc_b64_carry_n;

    /* Incoming — streaming b64 decode + LZ4F decompress */
    LZ4F_decompressionContext_t dec_ctx;
    int                         dec_active;
    /* b64 carry: 0..3 chars that didn't form a complete quartet yet. */
    char                        dec_b64_carry[4];
    uint8_t                     dec_b64_carry_n;
};

/*===========================================================================
 * Streaming base64 — encoder
 *
 * Standard "+/=" alphabet. Holds 0..2 bytes of input between calls. Each
 * call drains as many complete triples as possible into the destination
 * buffer; the leftover (size%3) goes into carry. encode_flush emits the
 * final partial as 2 or 3 chars + '=' padding.
 *=========================================================================*/

static const char b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static struct yetty_ycore_void_result
b64_emit_triple(struct yetty_ycore_buffer *out,
                uint8_t a, uint8_t b, uint8_t c)
{
    char chars[4];
    uint32_t v = ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c;
    chars[0] = b64_alpha[(v >> 18) & 0x3F];
    chars[1] = b64_alpha[(v >> 12) & 0x3F];
    chars[2] = b64_alpha[(v >>  6) & 0x3F];
    chars[3] = b64_alpha[ v        & 0x3F];
    return yetty_ycore_buffer_write(out, chars, 4);
}

/* Push len bytes through the encoder. Carry is updated. */
static struct yetty_ycore_void_result
b64_encode_push(struct yetty_yface *y, const uint8_t *src, size_t len)
{
    /* Combine carry + new input into triples. */
    size_t i = 0;
    while (y->enc_b64_carry_n > 0 && i < len) {
        if (y->enc_b64_carry_n == 1 && i + 1 < len) {
            struct yetty_ycore_void_result r = b64_emit_triple(
                &y->out_buf, y->enc_b64_carry[0], src[i], src[i + 1]);
            if (!r.ok) return r;
            i += 2;
            y->enc_b64_carry_n = 0;
        } else if (y->enc_b64_carry_n == 2 && i < len) {
            struct yetty_ycore_void_result r = b64_emit_triple(
                &y->out_buf, y->enc_b64_carry[0], y->enc_b64_carry[1], src[i]);
            if (!r.ok) return r;
            i += 1;
            y->enc_b64_carry_n = 0;
        } else {
            /* Need more bytes to complete a triple. */
            break;
        }
    }
    /* Whole triples from src[i..]. */
    while (i + 3 <= len) {
        struct yetty_ycore_void_result r = b64_emit_triple(
            &y->out_buf, src[i], src[i + 1], src[i + 2]);
        if (!r.ok) return r;
        i += 3;
    }
    /* Leftover into carry. */
    while (i < len && y->enc_b64_carry_n < 2) {
        y->enc_b64_carry[y->enc_b64_carry_n++] = src[i++];
    }
    /* If we still have input here something went wrong with the carry math. */
    if (i != len) return YETTY_ERR(yetty_ycore_void, "b64 carry overflow");
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
b64_encode_flush(struct yetty_yface *y)
{
    if (y->enc_b64_carry_n == 0) return YETTY_OK_VOID();

    char chars[4];
    if (y->enc_b64_carry_n == 1) {
        uint32_t v = (uint32_t)y->enc_b64_carry[0] << 16;
        chars[0] = b64_alpha[(v >> 18) & 0x3F];
        chars[1] = b64_alpha[(v >> 12) & 0x3F];
        chars[2] = '=';
        chars[3] = '=';
    } else { /* 2 */
        uint32_t v = ((uint32_t)y->enc_b64_carry[0] << 16) |
                     ((uint32_t)y->enc_b64_carry[1] << 8);
        chars[0] = b64_alpha[(v >> 18) & 0x3F];
        chars[1] = b64_alpha[(v >> 12) & 0x3F];
        chars[2] = b64_alpha[(v >>  6) & 0x3F];
        chars[3] = '=';
    }
    y->enc_b64_carry_n = 0;
    return yetty_ycore_buffer_write(&y->out_buf, chars, 4);
}

/*===========================================================================
 * Streaming base64 — decoder
 *
 * Holds 0..3 input chars between calls; emits raw bytes as soon as a
 * complete quartet is formed. Stops at first '=' (RFC-4648 padding).
 *=========================================================================*/

static const signed char b64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/* Decode one quartet of 4 valid (non-pad) b64 chars into 3 raw bytes. */
static int
b64_decode_quartet(const char *in, uint8_t out[3])
{
    int v0 = b64_table[(unsigned char)in[0]];
    int v1 = b64_table[(unsigned char)in[1]];
    int v2 = b64_table[(unsigned char)in[2]];
    int v3 = b64_table[(unsigned char)in[3]];
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return 0;
    uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                 ((uint32_t)v2 <<  6) |  (uint32_t)v3;
    out[0] = (uint8_t)((v >> 16) & 0xFF);
    out[1] = (uint8_t)((v >>  8) & 0xFF);
    out[2] = (uint8_t)( v        & 0xFF);
    return 1;
}

/*===========================================================================
 * The LZ4-out → b64-out chain that both write paths feed
 *=========================================================================*/

static struct yetty_ycore_void_result
emit_compressed_chunk(struct yetty_yface *y, size_t lz4_out_n)
{
    if (lz4_out_n == 0) return YETTY_OK_VOID();
    return b64_encode_push(y, y->enc_scratch, lz4_out_n);
}

/*===========================================================================
 * Outgoing API
 *=========================================================================*/

static struct yetty_ycore_void_result
ensure_enc_scratch(struct yetty_yface *y, size_t need)
{
    if (need <= y->enc_scratch_cap) return YETTY_OK_VOID();
    size_t new_cap = y->enc_scratch_cap ? y->enc_scratch_cap : 1024;
    while (new_cap < need) new_cap *= 2;
    uint8_t *p = (uint8_t *)realloc(y->enc_scratch, new_cap);
    if (!p) return YETTY_ERR(yetty_ycore_void, "scratch realloc failed");
    y->enc_scratch = p;
    y->enc_scratch_cap = new_cap;
    return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_yface_start_write(struct yetty_yface *y, int osc_code, const char *prefix)
{
    if (!y) return YETTY_ERR(yetty_ycore_void, "yface is NULL");
    if (y->enc_active)
        return YETTY_ERR(yetty_ycore_void, "yface: write already active");

    /* Raw OSC code prefix straight into out_buf: "\e]<code>;" */
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "\033]%d;", osc_code);
    if (n <= 0 || (size_t)n >= sizeof(hdr))
        return YETTY_ERR(yetty_ycore_void, "yface: bad osc_code");
    {
        struct yetty_ycore_void_result r =
            yetty_ycore_buffer_write(&y->out_buf, hdr, (size_t)n);
        if (!r.ok) return r;
    }

    /* Optional verb / arg prefix: "<prefix>;" — caller-supplied, written
     * raw so existing OSC argument parsers see the same shape they did
     * before yface (`<verb>[ <args...>];<base64-body>`). */
    if (prefix && prefix[0]) {
        size_t plen = strlen(prefix);
        struct yetty_ycore_void_result r =
            yetty_ycore_buffer_write(&y->out_buf, prefix, plen);
        if (!r.ok) return r;
        r = yetty_ycore_buffer_write(&y->out_buf, ";", 1);
        if (!r.ok) return r;
    }

    /* Reset b64 carry. */
    y->enc_b64_carry_n = 0;

    /* Allocate / reuse scratch sized for one LZ4 block worth of output. */
    {
        struct yetty_ycore_void_result r =
            ensure_enc_scratch(y, ENC_SCRATCH_CAP_DEFAULT);
        if (!r.ok) return r;
    }

    /* Spin up an LZ4 frame. */
    LZ4F_errorCode_t err =
        LZ4F_createCompressionContext(&y->enc_ctx, LZ4F_VERSION);
    if (LZ4F_isError(err))
        return YETTY_ERR(yetty_ycore_void, LZ4F_getErrorName(err));

    LZ4F_preferences_t prefs = {0};
    /* Default block size (64 KB), no checksum (we don't need one — yface's
     * out_buf is taken as a unit and OSC framing already provides terminators). */
    size_t hn = LZ4F_compressBegin(y->enc_ctx,
                                   y->enc_scratch, y->enc_scratch_cap, &prefs);
    if (LZ4F_isError(hn)) {
        LZ4F_freeCompressionContext(y->enc_ctx);
        y->enc_ctx = NULL;
        return YETTY_ERR(yetty_ycore_void, LZ4F_getErrorName(hn));
    }
    {
        struct yetty_ycore_void_result r = emit_compressed_chunk(y, hn);
        if (!r.ok) {
            LZ4F_freeCompressionContext(y->enc_ctx);
            y->enc_ctx = NULL;
            return r;
        }
    }

    y->enc_active = 1;
    return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_yface_write(struct yetty_yface *y, const void *src, size_t len)
{
    if (!y) return YETTY_ERR(yetty_ycore_void, "yface is NULL");
    if (!y->enc_active)
        return YETTY_ERR(yetty_ycore_void, "yface: write outside frame");
    if (len == 0) return YETTY_OK_VOID();
    if (!src)     return YETTY_ERR(yetty_ycore_void, "src is NULL");

    /* Worst case for THIS call's compressed output. */
    size_t bound = LZ4F_compressBound(len, NULL);
    {
        struct yetty_ycore_void_result r = ensure_enc_scratch(y, bound);
        if (!r.ok) return r;
    }
    size_t out_n = LZ4F_compressUpdate(y->enc_ctx,
                                       y->enc_scratch, y->enc_scratch_cap,
                                       src, len, NULL);
    if (LZ4F_isError(out_n))
        return YETTY_ERR(yetty_ycore_void, LZ4F_getErrorName(out_n));
    return emit_compressed_chunk(y, out_n);
}

struct yetty_ycore_void_result
yetty_yface_finish_write(struct yetty_yface *y)
{
    if (!y) return YETTY_ERR(yetty_ycore_void, "yface is NULL");
    if (!y->enc_active)
        return YETTY_ERR(yetty_ycore_void, "yface: no active write");

    /* compressEnd needs room for footer + buffered last block. */
    size_t bound = LZ4F_compressBound(0, NULL);
    {
        struct yetty_ycore_void_result r = ensure_enc_scratch(y, bound);
        if (!r.ok) return r;
    }
    size_t end_n = LZ4F_compressEnd(y->enc_ctx,
                                    y->enc_scratch, y->enc_scratch_cap, NULL);
    if (LZ4F_isError(end_n)) {
        LZ4F_freeCompressionContext(y->enc_ctx);
        y->enc_ctx = NULL;
        y->enc_active = 0;
        return YETTY_ERR(yetty_ycore_void, LZ4F_getErrorName(end_n));
    }
    {
        struct yetty_ycore_void_result r = emit_compressed_chunk(y, end_n);
        if (!r.ok) goto cleanup;
        r = b64_encode_flush(y);
        if (!r.ok) goto cleanup;
        /* OSC ST terminator. */
        r = yetty_ycore_buffer_write(&y->out_buf, "\033\\", 2);
        if (!r.ok) goto cleanup;
    }
    LZ4F_freeCompressionContext(y->enc_ctx);
    y->enc_ctx = NULL;
    y->enc_active = 0;
    return YETTY_OK_VOID();

cleanup:
    LZ4F_freeCompressionContext(y->enc_ctx);
    y->enc_ctx = NULL;
    y->enc_active = 0;
    return YETTY_ERR(yetty_ycore_void, "yface: finish_write failed");
}

/*===========================================================================
 * Incoming API
 *=========================================================================*/

struct yetty_ycore_void_result
yetty_yface_start_read(struct yetty_yface *y)
{
    if (!y) return YETTY_ERR(yetty_ycore_void, "yface is NULL");
    if (y->dec_active)
        return YETTY_ERR(yetty_ycore_void, "yface: read already active");

    LZ4F_errorCode_t err =
        LZ4F_createDecompressionContext(&y->dec_ctx, LZ4F_VERSION);
    if (LZ4F_isError(err))
        return YETTY_ERR(yetty_ycore_void, LZ4F_getErrorName(err));

    yetty_ycore_buffer_clear(&y->in_buf);
    y->dec_b64_carry_n = 0;
    y->dec_active = 1;
    return YETTY_OK_VOID();
}

/* Push raw bytes from b64 decode into LZ4F_decompress, append output to
 * in_buf. */
static struct yetty_ycore_void_result
dec_feed_compressed(struct yetty_yface *y, const uint8_t *bytes, size_t n)
{
    /* LZ4F_decompress consumes input and produces output; we may need to
     * loop until all input is consumed. The output is sized
     * incrementally: we ensure room, decompress, append. Reuse a small
     * stack scratch since LZ4F itself manages the dictionary. */
    uint8_t scratch[16 * 1024];
    size_t in_pos = 0;
    while (in_pos < n) {
        size_t in_left = n - in_pos;
        size_t out_left = sizeof(scratch);
        size_t r = LZ4F_decompress(y->dec_ctx, scratch, &out_left,
                                   bytes + in_pos, &in_left, NULL);
        if (LZ4F_isError(r))
            return YETTY_ERR(yetty_ycore_void, LZ4F_getErrorName(r));
        if (out_left > 0) {
            struct yetty_ycore_void_result rr =
                yetty_ycore_buffer_write(&y->in_buf, scratch, out_left);
            if (!rr.ok) return rr;
        }
        in_pos += in_left;
        if (in_left == 0 && out_left == 0) break; /* nothing happened */
    }
    return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_yface_feed(struct yetty_yface *y, const char *b64, size_t n)
{
    if (!y) return YETTY_ERR(yetty_ycore_void, "yface is NULL");
    if (!y->dec_active)
        return YETTY_ERR(yetty_ycore_void, "yface: feed outside read");
    if (n == 0) return YETTY_OK_VOID();
    if (!b64)   return YETTY_ERR(yetty_ycore_void, "b64 is NULL");

    /* Skip any chars after the first '=' — that's the end-of-stream marker. */
    size_t valid_n = 0;
    while (valid_n < n && b64[valid_n] != '=') valid_n++;
    /* (We accept '=' chars in carry too; b64_decode_quartet rejects them.) */

    /* Stream out complete quartets. Allocate a small temp byte buffer for
     * whatever we decode this call; feed it to LZ4F in one go. */
    /* Worst case: ((carry_n + valid_n) / 4) * 3 bytes. */
    size_t total_in = (size_t)y->dec_b64_carry_n + valid_n;
    size_t bytes_cap = (total_in / 4) * 3;
    if (bytes_cap == 0) {
        /* Just stash what we got into carry and return. */
        for (size_t i = 0; i < valid_n; i++) {
            if (y->dec_b64_carry_n >= 4)
                return YETTY_ERR(yetty_ycore_void, "yface: b64 carry overflow");
            y->dec_b64_carry[y->dec_b64_carry_n++] = b64[i];
        }
        return YETTY_OK_VOID();
    }

    uint8_t *bytes = (uint8_t *)malloc(bytes_cap);
    if (!bytes) return YETTY_ERR(yetty_ycore_void, "yface: oom");
    size_t bytes_n = 0;

    /* Drain carry first by combining with new chars to form quartets. */
    size_t pos = 0;
    while (y->dec_b64_carry_n > 0 && y->dec_b64_carry_n < 4 && pos < valid_n) {
        y->dec_b64_carry[y->dec_b64_carry_n++] = b64[pos++];
    }
    if (y->dec_b64_carry_n == 4) {
        uint8_t triple[3];
        if (b64_decode_quartet(y->dec_b64_carry, triple)) {
            memcpy(bytes + bytes_n, triple, 3);
            bytes_n += 3;
        } /* else silently drop — caller fed garbage */
        y->dec_b64_carry_n = 0;
    }

    /* Whole quartets straight from b64[pos..]. */
    while (pos + 4 <= valid_n) {
        uint8_t triple[3];
        if (b64_decode_quartet(b64 + pos, triple)) {
            memcpy(bytes + bytes_n, triple, 3);
            bytes_n += 3;
        }
        pos += 4;
    }

    /* Leftover (<=3 chars) into carry. */
    while (pos < valid_n) {
        if (y->dec_b64_carry_n >= 4) break;
        y->dec_b64_carry[y->dec_b64_carry_n++] = b64[pos++];
    }

    struct yetty_ycore_void_result r = dec_feed_compressed(y, bytes, bytes_n);
    free(bytes);
    return r;
}

struct yetty_ycore_void_result
yetty_yface_finish_read(struct yetty_yface *y)
{
    if (!y) return YETTY_ERR(yetty_ycore_void, "yface is NULL");
    if (!y->dec_active) return YETTY_OK_VOID();

    /* Decode any remaining carry that includes valid chars only — full
     * quartet (carry_n==4) is impossible at this point since feed would
     * have drained it. With 1..3 chars left there's no complete byte so
     * we simply discard the carry; LZ4F_compressEnd on the producer side
     * inserts proper end-of-frame so the decoder doesn't need any tail
     * input. */
    y->dec_b64_carry_n = 0;
    LZ4F_freeDecompressionContext(y->dec_ctx);
    y->dec_ctx = NULL;
    y->dec_active = 0;
    return YETTY_OK_VOID();
}

/*===========================================================================
 * Lifecycle + accessors
 *=========================================================================*/

struct yetty_yface_ptr_result yetty_yface_create(void)
{
    struct yetty_yface *y = calloc(1, sizeof(*y));
    if (!y) return YETTY_ERR(yetty_yface_ptr, "yface alloc failed");
    return YETTY_OK(yetty_yface_ptr, y);
}

void yetty_yface_destroy(struct yetty_yface *y)
{
    if (!y) return;
    if (y->enc_ctx) {
        LZ4F_freeCompressionContext(y->enc_ctx);
        y->enc_ctx = NULL;
    }
    if (y->dec_ctx) {
        LZ4F_freeDecompressionContext(y->dec_ctx);
        y->dec_ctx = NULL;
    }
    free(y->enc_scratch);
    yetty_ycore_buffer_destroy(&y->in_buf);
    yetty_ycore_buffer_destroy(&y->out_buf);
    free(y);
}

struct yetty_ycore_buffer *yetty_yface_in_buf(struct yetty_yface *y)
{
    return y ? &y->in_buf : NULL;
}

struct yetty_ycore_buffer *yetty_yface_out_buf(struct yetty_yface *y)
{
    return y ? &y->out_buf : NULL;
}

/*===========================================================================
 * One-shot helpers — see yface.h. Internal pattern: spin a transient
 * yface for the call, do the streaming op once, copy result, free.
 *=========================================================================*/

#include <unistd.h>
#include <errno.h>

struct yetty_ycore_void_result
yetty_yface_emit(int osc_code, const char *prefix,
                 const void *body, size_t body_len,
                 struct yetty_ycore_buffer *out_buf)
{
    if (!out_buf) return YETTY_ERR(yetty_ycore_void, "out_buf is NULL");

    struct yetty_yface_ptr_result yr = yetty_yface_create();
    if (YETTY_IS_ERR(yr))
        return YETTY_ERR(yetty_ycore_void, yr.error.msg);
    struct yetty_yface *y = yr.value;

    struct yetty_ycore_void_result r;
    r = yetty_yface_start_write(y, osc_code, prefix);
    if (YETTY_IS_ERR(r)) goto out;
    if (body && body_len) {
        r = yetty_yface_write(y, body, body_len);
        if (YETTY_IS_ERR(r)) goto out;
    }
    r = yetty_yface_finish_write(y);
    if (YETTY_IS_ERR(r)) goto out;

    /* Append the assembled OSC sequence to the caller's buffer. */
    r = yetty_ycore_buffer_write(out_buf, y->out_buf.data, y->out_buf.size);

out:
    yetty_yface_destroy(y);
    return r;
}

struct yetty_ycore_void_result
yetty_yface_emit_to_fd(int fd, int osc_code, const char *prefix,
                       const void *body, size_t body_len)
{
    struct yetty_ycore_buffer buf = {0};
    struct yetty_ycore_void_result r =
        yetty_yface_emit(osc_code, prefix, body, body_len, &buf);
    if (YETTY_IS_ERR(r)) {
        yetty_ycore_buffer_destroy(&buf);
        return r;
    }
    /* Blocking write_all — caller asked for the convenience function. */
    size_t off = 0;
    while (off < buf.size) {
        ssize_t w = write(fd, buf.data + off, buf.size - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            yetty_ycore_buffer_destroy(&buf);
            return YETTY_ERR(yetty_ycore_void, "write failed");
        }
        off += (size_t)w;
    }
    yetty_ycore_buffer_destroy(&buf);
    return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_yface_decode(const char *b64, size_t n,
                   struct yetty_ycore_buffer *out_buf)
{
    if (!out_buf) return YETTY_ERR(yetty_ycore_void, "out_buf is NULL");

    struct yetty_yface_ptr_result yr = yetty_yface_create();
    if (YETTY_IS_ERR(yr))
        return YETTY_ERR(yetty_ycore_void, yr.error.msg);
    struct yetty_yface *y = yr.value;

    struct yetty_ycore_void_result r;
    r = yetty_yface_start_read(y);
    if (YETTY_IS_ERR(r)) goto out;
    r = yetty_yface_feed(y, b64, n);
    if (YETTY_IS_ERR(r)) goto out;
    r = yetty_yface_finish_read(y);
    if (YETTY_IS_ERR(r)) goto out;

    r = yetty_ycore_buffer_write(out_buf, y->in_buf.data, y->in_buf.size);

out:
    yetty_yface_destroy(y);
    return r;
}
