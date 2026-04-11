#include <yetty/term/ypaint-layer.h>
#include <yetty/term/osc-args.h>
#include <yetty/ypaint/core/ypaint-canvas.h>
#include <yetty/ypaint/sdf/ypaint-sdf-yaml.gen.h>
#include <yetty/render/gpu-resource-set.h>
#include <yetty/util.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define INCBIN_STYLE 1
#include <incbin.h>

/* Embedded shader code */
INCBIN(ypaint_layer_shader, YPAINT_LAYER_SHADER_PATH);

/* Uniform positions */
#define U_GRID_SIZE       0
#define U_CELL_SIZE       1
#define U_ROW_ORIGIN      2
#define U_PRIM_COUNT      3
#define U_COUNT           4

/* Setters */
static inline void set_grid_size(struct yetty_render_gpu_resource_set *rs, float cols, float rows) {
    rs->uniforms[U_GRID_SIZE].vec2[0] = cols;
    rs->uniforms[U_GRID_SIZE].vec2[1] = rows;
}
static inline void set_cell_size(struct yetty_render_gpu_resource_set *rs, float w, float h) {
    rs->uniforms[U_CELL_SIZE].vec2[0] = w;
    rs->uniforms[U_CELL_SIZE].vec2[1] = h;
}
static inline void set_row_origin(struct yetty_render_gpu_resource_set *rs, uint32_t row_origin) {
    rs->uniforms[U_ROW_ORIGIN].u32 = row_origin;
}
static inline void set_prim_count(struct yetty_render_gpu_resource_set *rs, uint32_t count) {
    rs->uniforms[U_PRIM_COUNT].u32 = count;
}

/* Init uniforms */
static void init_uniforms(struct yetty_render_gpu_resource_set *rs)
{
    rs->uniform_count = U_COUNT;

    rs->uniforms[U_GRID_SIZE]  = (struct yetty_render_uniform){"ypaint_grid_size",  YETTY_RENDER_UNIFORM_VEC2};
    rs->uniforms[U_CELL_SIZE]  = (struct yetty_render_uniform){"ypaint_cell_size",  YETTY_RENDER_UNIFORM_VEC2};
    rs->uniforms[U_ROW_ORIGIN] = (struct yetty_render_uniform){"ypaint_row_origin", YETTY_RENDER_UNIFORM_U32};
    rs->uniforms[U_PRIM_COUNT] = (struct yetty_render_uniform){"ypaint_prim_count", YETTY_RENDER_UNIFORM_U32};

    set_row_origin(rs, 0);
    set_prim_count(rs, 0);
}

/* YPaint layer - embeds base as first member */
struct yetty_term_ypaint_layer {
    struct yetty_term_terminal_layer base;
    struct ypaint_canvas *canvas;
    int scrolling_mode;
    struct yetty_render_gpu_resource_set rs;

    /* Staging buffers - point to canvas data */
    uint8_t *grid_staging;
    size_t grid_staging_size;
    uint8_t *prim_staging;
    size_t prim_staging_size;
};

/* Forward declarations */
static void ypaint_layer_destroy(struct yetty_term_terminal_layer *self);
static void ypaint_layer_write(struct yetty_term_terminal_layer *self,
                               const char *data, size_t len);
static void ypaint_layer_resize(struct yetty_term_terminal_layer *self,
                                uint32_t cols, uint32_t rows);
static struct yetty_render_gpu_resource_set_result ypaint_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self);
static int ypaint_layer_on_key(struct yetty_term_terminal_layer *self, int key, int mods);
static int ypaint_layer_on_char(struct yetty_term_terminal_layer *self, uint32_t codepoint, int mods);
static int ypaint_layer_is_empty(const struct yetty_term_terminal_layer *self);
static void ypaint_layer_scroll(struct yetty_term_terminal_layer *self, int lines);
static void ypaint_layer_set_cursor(struct yetty_term_terminal_layer *self, int col, int row);

/* Ops */
static const struct yetty_term_terminal_layer_ops ypaint_layer_ops = {
    .destroy = ypaint_layer_destroy,
    .write = ypaint_layer_write,
    .resize = ypaint_layer_resize,
    .get_gpu_resource_set = ypaint_layer_get_gpu_resource_set,
    .is_empty = ypaint_layer_is_empty,
    .on_key = ypaint_layer_on_key,
    .on_char = ypaint_layer_on_char,
    .scroll = ypaint_layer_scroll,
    .set_cursor = ypaint_layer_set_cursor,
};

/* Create */
struct yetty_term_terminal_layer_result yetty_term_ypaint_layer_create(
    uint32_t cols, uint32_t rows,
    float cell_width, float cell_height,
    int scrolling_mode,
    yetty_term_request_render_fn request_render_fn,
    void *request_render_userdata,
    yetty_term_scroll_fn scroll_fn,
    void *scroll_userdata,
    yetty_term_cursor_fn cursor_fn,
    void *cursor_userdata)
{
    struct yetty_term_ypaint_layer *layer;

    layer = calloc(1, sizeof(struct yetty_term_ypaint_layer));
    if (!layer)
        return YETTY_ERR(yetty_term_terminal_layer, "failed to allocate ypaint layer");

    layer->base.ops = &ypaint_layer_ops;
    layer->base.cols = cols;
    layer->base.rows = rows;
    layer->base.cell_width = cell_width;
    layer->base.cell_height = cell_height;
    layer->base.dirty = 0;
    layer->base.pty_write_fn = NULL;  /* ypaint layer doesn't write to PTY */
    layer->base.pty_write_userdata = NULL;
    layer->base.request_render_fn = request_render_fn;
    layer->base.request_render_userdata = request_render_userdata;
    layer->base.scroll_fn = scroll_fn;
    layer->base.scroll_userdata = scroll_userdata;
    layer->base.cursor_fn = cursor_fn;
    layer->base.cursor_userdata = cursor_userdata;

    layer->scrolling_mode = scrolling_mode;

    /* Create canvas */
    layer->canvas = ypaint_canvas_create(scrolling_mode ? true : false);
    if (!layer->canvas) {
        free(layer);
        return YETTY_ERR(yetty_term_terminal_layer, "failed to create ypaint canvas");
    }

    /* Configure canvas scene bounds to match grid */
    float scene_width = (float)cols * cell_width;
    float scene_height = (float)rows * cell_height;
    ypaint_canvas_set_scene_bounds(layer->canvas, 0.0f, 0.0f, scene_width, scene_height);
    ypaint_canvas_set_cell_size(layer->canvas, cell_width, cell_height);

    /* Resource set */
    strncpy(layer->rs.namespace, scrolling_mode ? "ypaint_scroll" : "ypaint_overlay",
            YETTY_RENDER_NAME_MAX - 1);

    /* Buffer 0: grid staging (cell-to-primitive lookup) */
    layer->rs.buffer_count = 2;
    strncpy(layer->rs.buffers[0].name, "grid", YETTY_RENDER_NAME_MAX - 1);
    strncpy(layer->rs.buffers[0].wgsl_type, "array<u32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
    layer->rs.buffers[0].readonly = 1;

    /* Buffer 1: primitive staging (serialized primitives) */
    strncpy(layer->rs.buffers[1].name, "prims", YETTY_RENDER_NAME_MAX - 1);
    strncpy(layer->rs.buffers[1].wgsl_type, "array<u32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
    layer->rs.buffers[1].readonly = 1;

    init_uniforms(&layer->rs);
    set_grid_size(&layer->rs, (float)cols, (float)rows);
    set_cell_size(&layer->rs, cell_width, cell_height);

    yetty_render_shader_code_set(&layer->rs.shader,
        (const char *)gypaint_layer_shader_data, gypaint_layer_shader_size);

    ydebug("ypaint_layer_create: %s mode, %ux%u grid, %.1fx%.1f cells",
           scrolling_mode ? "scrolling" : "overlay", cols, rows, cell_width, cell_height);

    return YETTY_OK(yetty_term_terminal_layer, &layer->base);
}

/* Destroy */
static void ypaint_layer_destroy(struct yetty_term_terminal_layer *self)
{
    struct yetty_term_ypaint_layer *layer =
        (struct yetty_term_ypaint_layer *)self;

    if (layer->canvas)
        ypaint_canvas_destroy(layer->canvas);

    free(layer);
}

/* Write - receives ypaint data in format "args;payload" (base64 encoded) */
static void ypaint_layer_write(struct yetty_term_terminal_layer *self,
                               const char *data, size_t len)
{
    struct yetty_term_ypaint_layer *layer =
        (struct yetty_term_ypaint_layer *)self;
    struct yetty_term_osc_args args;

    if (!data || len == 0)
        return;

    /* Parse OSC args */
    if (yetty_term_osc_args_parse(&args, data, len) < 0) {
        yerror("ypaint_layer_write: failed to parse args");
        return;
    }

    /* Handle --clear */
    if (yetty_term_osc_args_has(&args, "clear")) {
        ydebug("ypaint_layer_write: clearing canvas");
        ypaint_canvas_clear(layer->canvas);
        yetty_term_osc_args_free(&args);
        layer->base.dirty = 1;
        if (layer->base.request_render_fn)
            layer->base.request_render_fn(layer->base.request_render_userdata);
        return;
    }

    /* Check for payload */
    if (!args.payload || args.payload_len == 0) {
        ydebug("ypaint_layer_write: no payload");
        yetty_term_osc_args_free(&args);
        return;
    }

    /* Base64 decode payload */
    size_t decoded_cap = args.payload_len;
    char *decoded = malloc(decoded_cap + 1);
    if (!decoded) {
        yerror("ypaint_layer_write: malloc failed");
        yetty_term_osc_args_free(&args);
        return;
    }

    size_t decoded_len = yetty_base64_decode(args.payload, args.payload_len, decoded, decoded_cap);
    decoded[decoded_len] = '\0';

    ydebug("ypaint_layer_write: yaml=%d payload_len=%zu decoded_len=%zu",
           yetty_term_osc_args_has(&args, "yaml"), args.payload_len, decoded_len);

    /* Handle --yaml format */
    if (yetty_term_osc_args_has(&args, "yaml")) {
        if (ypaint_sdf_yaml_parse(layer->canvas, decoded, decoded_len) < 0)
            yerror("ypaint_layer_write: yaml parse failed");
    } else {
        ydebug("ypaint_layer_write: binary format (not implemented)");
    }

    free(decoded);
    yetty_term_osc_args_free(&args);

    layer->base.dirty = 1;
    if (layer->base.request_render_fn)
        layer->base.request_render_fn(layer->base.request_render_userdata);
}

/* Resize */
static void ypaint_layer_resize(struct yetty_term_terminal_layer *self,
                                uint32_t cols, uint32_t rows)
{
    struct yetty_term_ypaint_layer *layer =
        (struct yetty_term_ypaint_layer *)self;

    self->cols = cols;
    self->rows = rows;

    /* Update canvas scene bounds */
    float scene_width = (float)cols * self->cell_width;
    float scene_height = (float)rows * self->cell_height;
    ypaint_canvas_set_scene_bounds(layer->canvas, 0.0f, 0.0f, scene_width, scene_height);

    set_grid_size(&layer->rs, (float)cols, (float)rows);
    self->dirty = 1;

    ydebug("ypaint_layer_resize: %ux%u", cols, rows);
}

/* Get GPU resource set */
static struct yetty_render_gpu_resource_set_result ypaint_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self)
{
    struct yetty_term_ypaint_layer *layer =
        (struct yetty_term_ypaint_layer *)self;

    if (layer->base.dirty || ypaint_canvas_is_dirty(layer->canvas)) {
        /* Rebuild grid staging */
        ypaint_canvas_rebuild_grid(layer->canvas);

        const uint32_t *grid_data = ypaint_canvas_grid_data(layer->canvas);
        uint32_t grid_word_count = ypaint_canvas_grid_word_count(layer->canvas);

        layer->rs.buffers[0].data = (uint8_t *)grid_data;
        layer->rs.buffers[0].size = grid_word_count * sizeof(uint32_t);
        layer->rs.buffers[0].dirty = 1;

        /* Build primitive staging */
        uint32_t prim_word_count = 0;
        const uint32_t *prim_data = ypaint_canvas_build_prim_staging(layer->canvas, &prim_word_count);

        layer->rs.buffers[1].data = (uint8_t *)prim_data;
        layer->rs.buffers[1].size = prim_word_count * sizeof(uint32_t);
        layer->rs.buffers[1].dirty = 1;

        /* Update uniforms */
        set_row_origin(&layer->rs, ypaint_canvas_row0_absolute(layer->canvas));
        uint32_t prim_count = ypaint_canvas_primitive_count(layer->canvas);
        set_prim_count(&layer->rs, prim_count);

        ydebug("ypaint_layer: grid=%u words, prims=%u words, prim_count=%u",
               grid_word_count, prim_word_count, prim_count);

        /* Debug: dump first primitive data */
        if (prim_data && prim_count > 0) {
            uint32_t offset0 = prim_data[0];
            ydebug("ypaint_layer: prim[0] offset=%u, type=%u, fill=0x%08x",
                   offset0, prim_data[prim_count + offset0],
                   prim_data[prim_count + offset0 + 2]);
            /* Dump geometry */
            ydebug("ypaint_layer: prim[0] geom: [5]=%f [6]=%f [7]=%f",
                   *(float*)&prim_data[prim_count + offset0 + 5],
                   *(float*)&prim_data[prim_count + offset0 + 6],
                   *(float*)&prim_data[prim_count + offset0 + 7]);
        }

        /* Debug: dump grid cell 0 */
        if (grid_data && grid_word_count > 0) {
            uint32_t cell0_start = grid_data[0];
            uint32_t cell0_count = grid_data[cell0_start];
            ydebug("ypaint_layer: grid[0] start=%u count=%u", cell0_start, cell0_count);
            if (cell0_count > 0) {
                ydebug("ypaint_layer: grid[0] prim_indices: %u %u %u",
                       grid_data[cell0_start + 1],
                       cell0_count > 1 ? grid_data[cell0_start + 2] : 0,
                       cell0_count > 2 ? grid_data[cell0_start + 3] : 0);
            }
        }
    }

    return YETTY_OK(yetty_render_gpu_resource_set, &layer->rs);
}

/* Keyboard input - ypaint layer doesn't handle keyboard */
static int ypaint_layer_on_key(struct yetty_term_terminal_layer *self, int key, int mods)
{
    (void)self;
    (void)key;
    (void)mods;
    return 0;  /* Not handled */
}

static int ypaint_layer_on_char(struct yetty_term_terminal_layer *self, uint32_t codepoint, int mods)
{
    (void)self;
    (void)codepoint;
    (void)mods;
    return 0;  /* Not handled */
}

/* YPaint layer is empty if there are no primitives */
static int ypaint_layer_is_empty(const struct yetty_term_terminal_layer *self)
{
    const struct yetty_term_ypaint_layer *layer =
        (const struct yetty_term_ypaint_layer *)self;

    if (!layer->canvas)
        return 1;

    return ypaint_canvas_primitive_count(layer->canvas) == 0;
}

/* Scroll - called when another layer scrolls */
static void ypaint_layer_scroll(struct yetty_term_terminal_layer *self, int lines)
{
    struct yetty_term_ypaint_layer *layer =
        (struct yetty_term_ypaint_layer *)self;

    if (!layer->canvas || !layer->scrolling_mode || lines <= 0)
        return;

    ypaint_canvas_scroll_lines(layer->canvas, (uint16_t)lines);
    layer->base.dirty = 1;

    ydebug("ypaint_layer_scroll: %d lines", lines);

    if (layer->base.request_render_fn)
        layer->base.request_render_fn(layer->base.request_render_userdata);
}

/* Set cursor - called when another layer moves cursor */
static void ypaint_layer_set_cursor(struct yetty_term_terminal_layer *self, int col, int row)
{
    struct yetty_term_ypaint_layer *layer =
        (struct yetty_term_ypaint_layer *)self;

    if (!layer->canvas)
        return;

    ypaint_canvas_set_cursor(layer->canvas, (uint16_t)col, (uint16_t)row);
    ydebug("ypaint_layer_set_cursor: col=%d row=%d", col, row);
}
