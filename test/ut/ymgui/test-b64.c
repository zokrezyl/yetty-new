/*
 * test-b64.c — correctness + microbench for the two base64 encoders we
 * have around: the frontend's `ymgui_b64_encode` (caller-allocates, no
 * malloc per call) and ycore's `yetty_ycore_base64_encode` (allocates a
 * result buffer per call).
 *
 * Tests:
 *   1. Round-trip on RFC 4648 vectors via ymgui_b64_encode + ycore decode.
 *   2. Both encoders produce identical output on random data.
 *   3. Microbench: 1 MB encoded N times, report ns/byte for each.
 *
 * The bench is the interesting one — perf reported `ymgui_b64_encode` at
 * ~12 % of demo CPU. The question this test answers: is that pure compute,
 * or is part of it the per-call malloc that ycore's API would impose?
 *
 * Build target: ut-ymgui-b64. Run: ctest -R ymgui_b64 (--verbose for nums).
 */

#include "ymgui_encode.h"

#include <yetty/ycore/util.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*===========================================================================
 * 1. RFC 4648 vectors
 *=========================================================================*/

struct vec { const char *raw; const char *b64; };
static const struct vec rfc4648[] = {
    { "",       ""         },
    { "f",      "Zg=="     },
    { "fo",     "Zm8="     },
    { "foo",    "Zm9v"     },
    { "foob",   "Zm9vYg==" },
    { "fooba",  "Zm9vYmE=" },
    { "foobar", "Zm9vYmFy" },
};

static void test_vectors(void)
{
    char out[64];
    for (size_t i = 0; i < sizeof(rfc4648)/sizeof(rfc4648[0]); i++) {
        size_t raw_len = strlen(rfc4648[i].raw);
        size_t expected_len = ymgui_b64_encoded_len(raw_len);
        size_t got = ymgui_b64_encode((const uint8_t *)rfc4648[i].raw,
                                      raw_len, out);
        out[got] = '\0';
        if (got != expected_len ||
            strcmp(out, rfc4648[i].b64) != 0) {
            fprintf(stderr,
                    "FAIL vec %zu: raw=%-8s expected=%-12s got=%s (got_len=%zu exp=%zu)\n",
                    i, rfc4648[i].raw, rfc4648[i].b64, out, got, expected_len);
            exit(1);
        }
    }
    printf("[ok] RFC 4648 vectors (%zu)\n",
           sizeof(rfc4648)/sizeof(rfc4648[0]));
}

/*===========================================================================
 * 2. Cross-check vs ycore on random data
 *=========================================================================*/

static void test_cross_check(void)
{
    enum { N = 256 * 1024 };
    uint8_t *raw = (uint8_t *)malloc(N);
    char    *out_a = (char *)malloc(ymgui_b64_encoded_len(N) + 1);
    assert(raw && out_a);
    /* Stable PRNG for reproducible failures. */
    uint32_t s = 0xDEADBEEFu;
    for (size_t i = 0; i < N; i++) {
        s = s * 1664525u + 1013904223u;
        raw[i] = (uint8_t)(s >> 24);
    }

    /* Encoder A: ymgui_b64_encode (caller-allocated). */
    size_t na = ymgui_b64_encode(raw, N, out_a);
    out_a[na] = '\0';

    /* Encoder B: ycore (mallocs). */
    struct yetty_ycore_buffer_result br = yetty_ycore_base64_encode(raw, N);
    if (!br.ok) {
        fprintf(stderr, "FAIL ycore encode err: %s\n", br.error.msg);
        exit(1);
    }

    if (br.value.size != na ||
        memcmp(out_a, br.value.data, na) != 0) {
        fprintf(stderr,
                "FAIL: ycore and ymgui differ. ymgui=%zu ycore=%zu\n",
                na, br.value.size);
        exit(1);
    }
    free(br.value.data);

    /* Round-trip via ycore decode. */
    uint8_t *back = (uint8_t *)malloc(N);
    size_t nb = yetty_ycore_base64_decode(out_a, na, (char *)back, N);
    if (nb != N || memcmp(back, raw, N) != 0) {
        fprintf(stderr, "FAIL: round-trip mismatch (decoded=%zu, want=%d)\n",
                nb, N);
        exit(1);
    }

    free(back);
    free(out_a);
    free(raw);
    printf("[ok] cross-check ymgui vs ycore + roundtrip on %d bytes\n", N);
}

/*===========================================================================
 * 3. Microbench
 *
 * Both encoders are exercised on the same buffer for the same number of
 * iterations. ymgui_b64_encode writes into a caller buffer reused across
 * iters; ycore_base64_encode mallocs+frees a fresh output each call,
 * which is the apples-to-apples cost the user asked about.
 *=========================================================================*/

static void bench(void)
{
    enum { SZ = 80 * 1024 };   /* a typical ymgui --frame body */
    uint32_t iters = 1024;     /* ~80 MB total per encoder */

    uint8_t *raw = (uint8_t *)malloc(SZ);
    char *scratch = (char *)malloc(ymgui_b64_encoded_len(SZ) + 1);
    assert(raw && scratch);
    for (size_t i = 0; i < SZ; i++) raw[i] = (uint8_t)(i * 31u + 7u);

    /* Warm caches for both. */
    (void)ymgui_b64_encode(raw, SZ, scratch);
    {
        struct yetty_ycore_buffer_result br =
            yetty_ycore_base64_encode(raw, SZ);
        if (br.ok) free(br.value.data);
    }

    /* ymgui_b64_encode — caller buffer, no per-call malloc. */
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < iters; i++) {
        size_t n = ymgui_b64_encode(raw, SZ, scratch);
        /* Defeat dead-code elimination. */
        scratch[0] = (char)(scratch[0] ^ (char)n);
    }
    uint64_t t1 = now_ns();

    /* ycore — mallocs+frees the output buffer every call. */
    uint64_t t2 = now_ns();
    for (uint32_t i = 0; i < iters; i++) {
        struct yetty_ycore_buffer_result br =
            yetty_ycore_base64_encode(raw, SZ);
        if (!br.ok) { fprintf(stderr, "FAIL ycore: %s\n", br.error.msg); exit(1); }
        scratch[0] = (char)(scratch[0] ^ (char)br.value.data[0]);
        free(br.value.data);
    }
    uint64_t t3 = now_ns();

    /* Bonus: ycore encode if we hand it a pre-warmed output buffer (i.e.
     * remove the alloc): simulate by mallocing+freeing OUTSIDE the timed
     * region — we still pay one malloc per call inside the API. There's
     * no API that takes a caller buffer, so this is the best we can do
     * without modifying ycore. We approximate the "compute only" cost
     * by subtracting an alloc-only loop. */
    uint64_t t4 = now_ns();
    for (uint32_t i = 0; i < iters; i++) {
        void *p = malloc(ymgui_b64_encoded_len(SZ) + 1);
        if (p) free(p);
    }
    uint64_t t5 = now_ns();

    double mb_per_iter = (double)SZ / (1024.0 * 1024.0);
    double total_mb    = mb_per_iter * (double)iters;
    double dt_ymgui_s  = (double)(t1 - t0) / 1e9;
    double dt_ycore_s  = (double)(t3 - t2) / 1e9;
    double dt_alloc_s  = (double)(t5 - t4) / 1e9;
    double dt_ycore_compute_s = dt_ycore_s - dt_alloc_s;
    if (dt_ycore_compute_s < 0) dt_ycore_compute_s = 0;

    printf("[bench] %u iters x %d B = %.1f MB total\n",
           iters, SZ, total_mb);
    printf("  ymgui_b64_encode (caller-buf):    %.3f s  (%.1f MB/s, %.2f ns/byte)\n",
           dt_ymgui_s, total_mb / dt_ymgui_s,
           dt_ymgui_s * 1e9 / ((double)SZ * iters));
    printf("  yetty_ycore_base64_encode:        %.3f s  (%.1f MB/s, %.2f ns/byte)\n",
           dt_ycore_s, total_mb / dt_ycore_s,
           dt_ycore_s * 1e9 / ((double)SZ * iters));
    printf("    of which malloc/free overhead:  %.3f s  (%.2f%% of ycore)\n",
           dt_alloc_s, 100.0 * dt_alloc_s / dt_ycore_s);
    printf("    of which pure compute (est):    %.3f s  (%.1f MB/s)\n",
           dt_ycore_compute_s,
           dt_ycore_compute_s > 0 ? total_mb / dt_ycore_compute_s : 0.0);

    free(scratch);
    free(raw);
}

int main(void)
{
    test_vectors();
    test_cross_check();
    bench();
    return 0;
}
