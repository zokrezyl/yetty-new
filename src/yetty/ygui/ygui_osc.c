/*
 * ygui_osc.c - OSC output to yetty terminal
 */

#include "ygui_internal.h"
#include <stdio.h>

#ifdef _WIN32
#include <io.h>
#define write _write
#define STDOUT_FILENO 1
#else
#include <unistd.h>
#endif

/* Base64 encoding table */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode data to base64 into output buffer. Returns bytes written. */
static size_t base64_encode(const uint8_t* data, uint32_t size, char* out, size_t out_size) {
    size_t out_len = ((size + 2) / 3) * 4;
    if (out_len + 1 > out_size) return 0;

    size_t j = 0;
    for (uint32_t i = 0; i < size; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < size) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < size) n |= (uint32_t)data[i + 2];

        out[j++] = b64_table[(n >> 18) & 0x3F];
        out[j++] = b64_table[(n >> 12) & 0x3F];
        out[j++] = (i + 1 < size) ? b64_table[(n >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < size) ? b64_table[n & 0x3F] : '=';
    }
    out[j] = '\0';
    return j;
}

/* Write OSC sequence to stdout */
static void write_osc(const char* data, size_t len) {
    ssize_t written = write(STDOUT_FILENO, data, len);
    (void)written; /* Ignore return value - best effort */
}

/* Vendor ID = yetty ypaint-layer OSC sink (see terminal.c register_osc_sink).
 * The layer's write() handler accepts:
 *   \033]666674;--clear\033\     — empty the canvas
 *   \033]666674;--bin;<base64>\033\ — append a base64 ypaint buffer
 *
 * ygui's semantics are "the whole UI is re-rendered every tick". We map
 * that onto the layer by sending --clear followed by --bin on every
 * create_card / update_card — the canvas ends up holding exactly the
 * latest UI each frame. Positioning args (-x/-y/-w/-h) are ignored: the
 * ypaint primitives already carry absolute pixel coords. */
#define VENDOR_ID "666674"

static void write_clear_and_bin(const uint8_t* data, uint32_t size) {
    /* 1) Clear the ypaint canvas. */
    static const char clear_seq[] = "\033]" VENDOR_ID ";--clear\033\\";
    write_osc(clear_seq, sizeof(clear_seq) - 1);

    if (size == 0 || !data) return;

    /* 2) Append base64(primitive bytes) as --bin. */
    size_t b64_size = ((size + 2) / 3) * 4 + 1;
    size_t buf_size = 64 + b64_size;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return;

    int header_len = snprintf(buf, 64, "\033]" VENDOR_ID ";--bin;");

    char* b64_start = buf + header_len;
    size_t b64_len = base64_encode(data, size, b64_start,
                                   buf_size - header_len - 3);

    b64_start[b64_len] = '\033';
    b64_start[b64_len + 1] = '\\';

    write_osc(buf, header_len + b64_len + 2);
    free(buf);
}

void ygui_osc_create_card(const char* name, int x, int y, int w, int h,
                          const uint8_t* data, uint32_t size) {
    (void)name; (void)x; (void)y; (void)w; (void)h;
    write_clear_and_bin(data, size);
}

void ygui_osc_update_card(const char* name, const uint8_t* data, uint32_t size) {
    (void)name;
    write_clear_and_bin(data, size);
}

void ygui_osc_kill_card(const char* name) {
    /* No named-card concept on this sink — kill = clear the canvas. */
    (void)name;
    static const char clear_seq[] = "\033]" VENDOR_ID ";--clear\033\\";
    write_osc(clear_seq, sizeof(clear_seq) - 1);
}

void ygui_osc_subscribe_clicks(int enable) {
    if (enable) {
        write_osc("\033[?1500h", 8);
    } else {
        write_osc("\033[?1500l", 8);
    }
}

void ygui_osc_subscribe_moves(int enable) {
    if (enable) {
        write_osc("\033[?1501h", 8);
    } else {
        write_osc("\033[?1501l", 8);
    }
}

void ygui_osc_query_cell_size(void) {
    /* CSI 16 t - request cell size in pixels */
    write_osc("\033[16t", 5);
}

void ygui_osc_subscribe_view_changes(int enable) {
    /* DEC mode 1502 - subscribe to card view change events */
    if (enable) {
        write_osc("\033[?1502h", 8);
    } else {
        write_osc("\033[?1502l", 8);
    }
}

void ygui_osc_zoom_card(const char* name, float level) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "\033]" VENDOR_ID ";zoom --name %s --level %.2f\033\\", name, level);
    write_osc(buf, len);
}

void ygui_osc_scroll_card(const char* name, float x, float y, int absolute) {
    char buf[256];
    int len;
    if (absolute) {
        /* Absolute scroll position: -x/-y */
        len = snprintf(buf, sizeof(buf),
            "\033]" VENDOR_ID ";scroll --name %s -x %.0f -y %.0f\033\\", name, x, y);
    } else {
        /* Relative scroll delta: --dx/--dy */
        len = snprintf(buf, sizeof(buf),
            "\033]" VENDOR_ID ";scroll --name %s --dx %.0f --dy %.0f\033\\", name, x, y);
    }
    write_osc(buf, len);
}

void ygui_osc_scroll_card_delta(const char* name, float dx, float dy) {
    ygui_osc_scroll_card(name, dx, dy, 0);
}
