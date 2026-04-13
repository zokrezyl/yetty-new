// ypaint-bench: Generate random SDF primitives as binary OSC sequence
// Outputs raw binary format (not YAML) for performance benchmarking

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// SDF primitive types (from ypaint-sdf-types.gen.h)
enum ypaint_sdf_type {
    YPAINT_SDF_CIRCLE = 0,
    YPAINT_SDF_BOX = 1,
    YPAINT_SDF_SEGMENT = 2,
    YPAINT_SDF_TRIANGLE = 3,
    YPAINT_SDF_ELLIPSE = 6,
    YPAINT_SDF_RHOMBUS = 9,
    YPAINT_SDF_PENTAGON = 10,
    YPAINT_SDF_HEXAGON = 11,
    YPAINT_SDF_STAR = 12,
    YPAINT_SDF_HEART = 15,
    YPAINT_SDF_HEXAGRAM = 29,
};

// Word counts per primitive type
static uint32_t prim_word_count(enum ypaint_sdf_type type) {
    switch (type) {
        case YPAINT_SDF_CIRCLE: return 8;
        case YPAINT_SDF_BOX: return 10;
        case YPAINT_SDF_SEGMENT: return 9;
        case YPAINT_SDF_TRIANGLE: return 11;
        case YPAINT_SDF_ELLIPSE: return 9;
        case YPAINT_SDF_RHOMBUS: return 9;
        case YPAINT_SDF_PENTAGON: return 8;
        case YPAINT_SDF_HEXAGON: return 8;
        case YPAINT_SDF_STAR: return 10;
        case YPAINT_SDF_HEART: return 8;
        case YPAINT_SDF_HEXAGRAM: return 8;
        default: return 0;
    }
}

// Available primitive types for random selection
static const enum ypaint_sdf_type PRIM_TYPES[] = {
    YPAINT_SDF_CIRCLE, YPAINT_SDF_BOX, YPAINT_SDF_SEGMENT, YPAINT_SDF_TRIANGLE,
    YPAINT_SDF_ELLIPSE, YPAINT_SDF_RHOMBUS, YPAINT_SDF_PENTAGON,
    YPAINT_SDF_HEXAGON, YPAINT_SDF_STAR, YPAINT_SDF_HEART, YPAINT_SDF_HEXAGRAM,
};
#define NUM_PRIM_TYPES (sizeof(PRIM_TYPES) / sizeof(PRIM_TYPES[0]))

// Base64 encoding
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *in, size_t in_len, char *out,
                            size_t out_cap) {
    size_t out_len = 0;
    for (size_t i = 0; i < in_len && out_len + 4 <= out_cap;) {
        uint32_t octet_a = i < in_len ? in[i++] : 0;
        uint32_t octet_b = i < in_len ? in[i++] : 0;
        uint32_t octet_c = i < in_len ? in[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[out_len++] = B64_TABLE[(triple >> 18) & 0x3F];
        out[out_len++] = B64_TABLE[(triple >> 12) & 0x3F];
        out[out_len++] = B64_TABLE[(triple >> 6) & 0x3F];
        out[out_len++] = B64_TABLE[triple & 0x3F];
    }
    // Padding
    size_t mod = in_len % 3;
    if (mod > 0 && out_len >= 1)
        out[out_len - 1] = '=';
    if (mod == 1 && out_len >= 2)
        out[out_len - 2] = '=';
    return out_len;
}

// Fast xorshift64 PRNG
static uint64_t g_rng_state = 1;

static inline uint64_t xorshift64(void) {
    uint64_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

static inline float randf(float min, float max) {
    return min + (float)(xorshift64() & 0xFFFFFF) / 16777215.0f * (max - min);
}

static inline uint32_t rand_color(void) {
    uint64_t r = xorshift64();
    return (uint32_t)((0x80 + (r & 0x7F)) << 24) | (uint32_t)(r >> 8);
}

static inline uint32_t randu32(void) {
    return (uint32_t)xorshift64();
}

// Write u32 as float bits
static void write_u32(float *buf, uint32_t val) {
    memcpy(buf, &val, sizeof(uint32_t));
}

// Generate single random primitive into buffer, returns words written
static uint32_t gen_primitive(float *buf, float scene_w, float scene_h) {
    (void)scene_h; // unused - we use cursor-relative coords
    enum ypaint_sdf_type type = PRIM_TYPES[randu32() % NUM_PRIM_TYPES];
    uint32_t word_count = prim_word_count(type);

    // Common header: type, z_order, fill, stroke, stroke_width
    write_u32(&buf[0], type);
    write_u32(&buf[1], randu32() % 100); // z_order
    write_u32(&buf[2], rand_color()); // fill
    write_u32(&buf[3], rand_color()); // stroke
    buf[4] = randf(0.5f, 3.0f);       // stroke_width

    // Cursor-relative coordinates - small Y values near cursor
    float cx = randf(0, scene_w);
    float cy = randf(10.0f, 150.0f);  // relative to cursor, not absolute
    float size = randf(10.0f, 50.0f);

    switch (type) {
    case YPAINT_SDF_CIRCLE:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size;
        break;
    case YPAINT_SDF_BOX:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size;           // half_width
        buf[8] = size * 0.7f;    // half_height
        buf[9] = randf(0, 10.0f); // corner_radius
        break;
    case YPAINT_SDF_SEGMENT:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = cx + randf(-100, 100);
        buf[8] = cy + randf(-100, 100);
        break;
    case YPAINT_SDF_TRIANGLE:
        buf[5] = cx;
        buf[6] = cy - size;
        buf[7] = cx - size;
        buf[8] = cy + size;
        buf[9] = cx + size;
        buf[10] = cy + size;
        break;
    case YPAINT_SDF_ELLIPSE:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size;
        buf[8] = size * 0.6f;
        break;
    case YPAINT_SDF_RHOMBUS:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size;
        buf[8] = size * 0.8f;
        break;
    case YPAINT_SDF_PENTAGON:
    case YPAINT_SDF_HEXAGON:
    case YPAINT_SDF_HEXAGRAM:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size;
        break;
    case YPAINT_SDF_STAR:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size;
        buf[8] = 5.0f + (float)(randu32() % 4); // num_points: 5-8
        buf[9] = randf(0.3f, 0.6f);          // inner_ratio
        break;
    case YPAINT_SDF_HEART:
        buf[5] = cx;
        buf[6] = cy;
        buf[7] = size * 0.5f;
        break;
    default:
        break;
    }

    return word_count;
}

// Buffer for primitives
#define MAX_BUFFER_SIZE (1024 * 1024) // 1MB
static float g_buffer[MAX_BUFFER_SIZE / sizeof(float)];

int main(int argc, char **argv) {
    uint32_t count = 100;
    uint32_t seed = 0;
    float scene_w = 800.0f;
    float scene_h = 600.0f;
    int loop_count = 1;  // 1 = single run, -1 = infinite, >1 = N iterations
    int delay_ms = 16; // ~60fps

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            seed = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            scene_w = atof(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            scene_h = atof(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--loop") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                loop_count = atoi(argv[++i]);
                if (loop_count <= 0) loop_count = -1;  // infinite
            } else {
                loop_count = -1;  // infinite
            }
        }
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            delay_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "Usage: %s [options]\n"
                    "  -n <count>  Number of primitives (default: 100)\n"
                    "  -s <seed>   Random seed (default: time-based)\n"
                    "  -w <width>  Scene width (default: 800)\n"
                    "  -h <height> Scene height (default: 600)\n"
                    "  -l [N]      Loop N times (default: infinite if no N)\n"
                    "  -d <ms>     Delay between frames in loop mode (default: 16)\n",
                    argv[0]);
            return 0;
        }
    }

    g_rng_state = seed ? seed : (uint64_t)time(NULL);

    do {
        // Generate primitives
        uint32_t total_words = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (total_words >= MAX_BUFFER_SIZE / sizeof(float) - 20)
                break;
            total_words += gen_primitive(&g_buffer[total_words], scene_w, scene_h);
        }

        // Base64 encode
        size_t raw_bytes = total_words * sizeof(float);
        size_t b64_cap = (raw_bytes + 2) / 3 * 4 + 1;
        char *b64 = malloc(b64_cap);
        if (!b64) {
            fprintf(stderr, "malloc failed\n");
            return 1;
        }

        size_t b64_len =
            base64_encode((uint8_t *)g_buffer, raw_bytes, b64, b64_cap);
        b64[b64_len] = '\0';

        // Output OSC sequence: ESC ] 666674 ; args ; payload ST
        printf("\033]666674;--bin;%s\033\\", b64);
        fflush(stdout);

        free(b64);

        if (loop_count != 1) {
            struct timespec ts = {.tv_sec = delay_ms / 1000,
                                  .tv_nsec = (delay_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
            g_rng_state++;
            if (loop_count > 1) loop_count--;
        }
    } while (loop_count != 1);

    return 0;
}
