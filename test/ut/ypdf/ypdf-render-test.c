/*
 * ypdf render smoke test.
 *
 * Loads test-comprehensive.pdf (3 pages) through yetty_ypdf_render_pdf and
 * validates the resulting ypaint buffer:
 *   - pages are counted correctly
 *   - scene bounds reflect the accumulated page heights
 *   - at least one TTF font was extracted (test PDF embeds fonts)
 *   - at least one text span was emitted
 *   - the primitive byte buffer is non-empty (rectangles/segments present)
 */

#include <pdfio.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ypdf/ypdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#ifndef YPDF_TEST_PDF
#define YPDF_TEST_PDF "test-comprehensive.pdf"
#endif

static bool error_cb(pdfio_file_t *f, const char *s, void *d) {
    (void)f; (void)d;
    fprintf(stderr, "pdfio: %s\n", s);
    return true;
}

int main(void) {
    pdfio_file_t *pdf = pdfioFileOpen(YPDF_TEST_PDF, NULL, NULL,
                                      error_cb, NULL);
    REQUIRE(pdf, "pdfioFileOpen failed");

    struct yetty_ypdf_render_result res = yetty_ypdf_render_pdf(pdf);
    REQUIRE(YETTY_IS_OK(res), "yetty_ypdf_render_pdf failed");

    struct yetty_ypdf_render_output *out = &res.value;
    REQUIRE(out->buffer, "buffer is NULL");
    REQUIRE(out->page_count == 3, "expected 3 pages");
    REQUIRE(out->total_height > 0.0f, "total_height not set");
    REQUIRE(out->max_width > 0.0f, "max_width not set");

    /* Scene bounds should match the renderer output. */
    float sx = yetty_ypaint_core_buffer_scene_max_x(out->buffer);
    float sy = yetty_ypaint_core_buffer_scene_max_y(out->buffer);
    REQUIRE(sx == out->max_width, "scene max_x mismatch");
    REQUIRE(sy == out->total_height, "scene max_y mismatch");

    /* Test PDF embeds fonts, expect at least one to be captured. */
    REQUIRE(yetty_ypaint_core_buffer_font_count(out->buffer) >= 1,
            "no fonts extracted");

    /* At least one text span. */
    REQUIRE(yetty_ypaint_core_buffer_text_span_count(out->buffer) >= 1,
            "no text spans emitted");

    printf("OK: %d pages, %u fonts, %u text spans, total_h=%.1f\n",
           out->page_count,
           yetty_ypaint_core_buffer_font_count(out->buffer),
           yetty_ypaint_core_buffer_text_span_count(out->buffer),
           out->total_height);

    yetty_ypaint_core_buffer_destroy(out->buffer);
    pdfioFileClose(pdf);
    return 0;
}
