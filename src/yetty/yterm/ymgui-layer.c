/*
 * ymgui-layer.c — cursor-anchored, scroll-synced Dear ImGui layer.
 *
 * RESPONSIBILITIES
 *   - Accept OSC (vendor 666680, verbs --frame / --tex / --clear).
 *   - Decode the wire format from include/yetty/ymgui/wire.h.
 *   - Anchor the frame at the cursor's rolling row on arrival (same model
 *     ypaint-canvas uses — cf. docs/ypaint.md "Rolling Offset Optimization").
 *   - When the frame would overflow the visible rows below the cursor,
 *     propagate scroll_fn so the terminal advances; the next --frame is
 *     placed under the new cursor position.
 *   - Propagate scroll/cursor fan-out from other layers: on scroll() we
 *     bump row0 (O(1)); on set_cursor() we just remember the cell for the
 *     next --frame's anchor.
 *
 * RENDERING
 *   A yetty layer is drawn as a fullscreen quad running a fragment shader
 *   that sees uniforms, storage buffers and textures from its resource set.
 *   We CPU-rasterize the ImGui triangle soup into an RGBA8 texture sized to
 *   the cards pixel grid, and the shader samples it with a scroll offset.
 *
 *   The rasterizer itself (ymgui_raster_*) is a small, self-contained unit
 *   at the bottom of this file — solid-color and alpha-textured triangles
 *   with clip rects. It is deliberately simple; optimising to GPU-side
 *   rendering is a separate effort that also needs a new yrender code path
 *   (indexed triangles + scissor) — documented as TODO there.
 *
 * OWNERSHIP
 *   layer owns: decoded frame bytes (copy of payload), texture slots, the
 *   rasterised RGBA8 image, shader source string, resource set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yetty/ycore/result.h>
#include <yetty/ycore/util.h>
#include <yetty/yconfig.h>
#include <yetty/yetty.h>
#include <yetty/ymgui/wire.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/yrender/render-target.h>
#include <yetty/yterm/osc-args.h>
#include <yetty/yterm/pty-reader.h>
#include <yetty/yterm/terminal.h>
#include <yetty/yterm/ymgui-layer.h>
#include <yetty/ytrace.h>

/*===========================================================================
 * Small helpers
 *=========================================================================*/

static inline float    f_min(float a, float b)         { return a < b ? a : b; }
static inline float    f_max(float a, float b)         { return a > b ? a : b; }
static inline float    f_clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*===========================================================================
 * Texture slots (atlas only for v1; extend by increasing MAX).
 *=========================================================================*/

#define YMGUI_MAX_TEX_SLOTS 4

struct ymgui_tex_slot {
    uint32_t tex_id;        /* 0 = unused */
    uint32_t format;        /* YMGUI_TEX_FMT_* */
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;        /* owned */
};

/*===========================================================================
 * Uniform slots (fragment shader contract — keep in sync with .wgsl).
 *=========================================================================*/

#define U_GRID_SIZE        0   /* vec2(cols, rows) */
#define U_CELL_SIZE        1   /* vec2(cell_w, cell_h) */
#define U_ROW_ORIGIN       2   /* u32, rolling row of top visible line */
#define U_FRAME_ROLLING    3   /* u32, rolling row where frame is anchored */
#define U_FRAME_SIZE       4   /* vec2(display_size_x, display_size_y) in px */
#define U_FRAME_PRESENT    5   /* u32, 1 if a raster exists and anchor visible */
#define U_VZ_SCALE         6   /* f32, visual zoom scale */
#define U_VZ_OFF           7   /* vec2, visual zoom offset */
#define U_COUNT            8

/*===========================================================================
 * Layer object
 *=========================================================================*/

struct yetty_yterm_ymgui_layer {
    struct yetty_yterm_terminal_layer base;

    /* Scrolling anchor model (same idea as yetty_ypaint_canvas). */
    uint32_t next_rolling_row;   /* monotonic; increments with every line
                                  * that scrolls past — here we only track
                                  * row0_absolute, so next_rolling_row ends
                                  * up as "last cursor rolling_row + 1" */
    uint32_t row0_absolute;      /* rolling row of top visible line */
    uint32_t cursor_row;         /* last cursor row (cell coords) */
    uint32_t cursor_col;

    /* Current frame. Copy of the decoded payload kept alive until the next
     * --frame or --clear (pointers inside frame_hdr view the copy). */
    uint8_t *frame_bytes;
    size_t   frame_size;
    int      has_frame;

    /* Decoded anchor. Set when frame arrives. */
    uint32_t frame_rolling_row;  /* rolling row where the frame sits */
    float    frame_display_w;    /* px */
    float    frame_display_h;

    /* Textures uploaded via --tex. */
    struct ymgui_tex_slot texs[YMGUI_MAX_TEX_SLOTS];

    /* CPU raster target: RGBA8 pixel buffer sized to the *frame*, not the
     * grid. The shader positions this rectangle inside the grid based on
     * (frame_rolling_row - row_origin)*cell_height, so scroll is O(1)
     * (uniform bump only — no re-rasterisation). */
    uint8_t *raster;
    uint32_t raster_w;
    uint32_t raster_h;
    int      raster_dirty;

    /* GPU side. */
    struct yetty_ycore_buffer shader_code;
    struct yetty_yrender_gpu_resource_set rs;
};

/*===========================================================================
 * Uniform setters
 *=========================================================================*/

static void init_uniforms(struct yetty_yrender_gpu_resource_set *rs)
{
    rs->uniform_count = U_COUNT;

    rs->uniforms[U_GRID_SIZE]     = (struct yetty_yrender_uniform){
        "ymgui_grid_size",     YETTY_YRENDER_UNIFORM_VEC2};
    rs->uniforms[U_CELL_SIZE]     = (struct yetty_yrender_uniform){
        "ymgui_cell_size",     YETTY_YRENDER_UNIFORM_VEC2};
    rs->uniforms[U_ROW_ORIGIN]    = (struct yetty_yrender_uniform){
        "ymgui_row_origin",    YETTY_YRENDER_UNIFORM_U32};
    rs->uniforms[U_FRAME_ROLLING] = (struct yetty_yrender_uniform){
        "ymgui_frame_rolling", YETTY_YRENDER_UNIFORM_U32};
    rs->uniforms[U_FRAME_SIZE]    = (struct yetty_yrender_uniform){
        "ymgui_frame_size",    YETTY_YRENDER_UNIFORM_VEC2};
    rs->uniforms[U_FRAME_PRESENT] = (struct yetty_yrender_uniform){
        "ymgui_frame_present", YETTY_YRENDER_UNIFORM_U32};
    rs->uniforms[U_VZ_SCALE]      = (struct yetty_yrender_uniform){
        "ymgui_vz_scale",      YETTY_YRENDER_UNIFORM_F32};
    rs->uniforms[U_VZ_OFF]        = (struct yetty_yrender_uniform){
        "ymgui_vz_off",        YETTY_YRENDER_UNIFORM_VEC2};

    rs->uniforms[U_ROW_ORIGIN].u32    = 0;
    rs->uniforms[U_FRAME_ROLLING].u32 = 0;
    rs->uniforms[U_FRAME_PRESENT].u32 = 0;
    rs->uniforms[U_VZ_SCALE].f32      = 1.0f;
    rs->uniforms[U_VZ_OFF].vec2[0]    = 0.0f;
    rs->uniforms[U_VZ_OFF].vec2[1]    = 0.0f;
}

/*===========================================================================
 * CPU raster (forward declarations — implementation at bottom of file)
 *=========================================================================*/

static void ymgui_raster_ensure(struct yetty_yterm_ymgui_layer *layer,
                                uint32_t w, uint32_t h);
static void ymgui_raster_clear(struct yetty_yterm_ymgui_layer *layer);
static void ymgui_raster_frame(struct yetty_yterm_ymgui_layer *layer);

/*===========================================================================
 * Wire decoding
 *=========================================================================*/

/* Decode base64 in-place; returns decoded length, or SIZE_MAX on malloc fail. */
static size_t decode_b64_alloc(const char *in, size_t in_len, uint8_t **out)
{
    /* ceil(len*3/4) upper bound */
    size_t cap = ((in_len + 3) / 4) * 3;
    uint8_t *buf = (uint8_t *)malloc(cap ? cap : 1);
    if (!buf) {
        *out = NULL;
        return (size_t)-1;
    }
    size_t n = yetty_ycore_base64_decode(in, in_len, (char *)buf, cap);
    *out = buf;
    return n;
}

/* Validate and extract frame view. Does NOT copy — caller owns frame_bytes. */
static int validate_frame(const uint8_t *data, size_t size,
                          const struct ymgui_wire_frame **out_hdr)
{
    if (size < sizeof(struct ymgui_wire_frame))
        return -1;
    const struct ymgui_wire_frame *fh = (const struct ymgui_wire_frame *)data;
    if (fh->magic != YMGUI_WIRE_MAGIC_FRAME) return -1;
    if (fh->version != YMGUI_WIRE_VERSION)   return -1;
    if (fh->total_size != size)              return -1;
    *out_hdr = fh;
    return 0;
}

static int validate_tex(const uint8_t *data, size_t size,
                        const struct ymgui_wire_tex **out_hdr)
{
    if (size < sizeof(struct ymgui_wire_tex))
        return -1;
    const struct ymgui_wire_tex *th = (const struct ymgui_wire_tex *)data;
    if (th->magic != YMGUI_WIRE_MAGIC_TEX)   return -1;
    if (th->version != YMGUI_WIRE_VERSION)   return -1;
    if (th->total_size != size)              return -1;
    uint32_t bpp = (th->format == YMGUI_TEX_FMT_R8) ? 1u
                 : (th->format == YMGUI_TEX_FMT_RGBA8) ? 4u : 0u;
    if (bpp == 0) return -1;
    size_t pixel_bytes = (size_t)th->width * (size_t)th->height * bpp;
    if ((size_t)th->total_size != sizeof(*th) + pixel_bytes) return -1;
    *out_hdr = th;
    return 0;
}

/*===========================================================================
 * Texture management
 *=========================================================================*/

static struct ymgui_tex_slot *
tex_find_or_alloc(struct yetty_yterm_ymgui_layer *layer, uint32_t tex_id)
{
    for (int i = 0; i < YMGUI_MAX_TEX_SLOTS; i++)
        if (layer->texs[i].tex_id == tex_id)
            return &layer->texs[i];
    for (int i = 0; i < YMGUI_MAX_TEX_SLOTS; i++)
        if (layer->texs[i].tex_id == 0)
            return &layer->texs[i];
    return NULL;
}

static struct ymgui_tex_slot *
tex_find(const struct yetty_yterm_ymgui_layer *layer, uint32_t tex_id)
{
    for (int i = 0; i < YMGUI_MAX_TEX_SLOTS; i++) {
        if (layer->texs[i].tex_id == tex_id)
            return (struct ymgui_tex_slot *)&layer->texs[i];
    }
    return NULL;
}

static void tex_clear_all(struct yetty_yterm_ymgui_layer *layer)
{
    for (int i = 0; i < YMGUI_MAX_TEX_SLOTS; i++) {
        free(layer->texs[i].pixels);
        layer->texs[i].pixels = NULL;
        layer->texs[i].tex_id = 0;
    }
}

/*===========================================================================
 * Frame anchoring / fit-under-cursor
 *=========================================================================*/

/* How many rows (ceil) the current frame occupies. */
static uint32_t frame_row_span(const struct yetty_yterm_ymgui_layer *layer)
{
    if (!layer->has_frame || layer->base.cell_size.height <= 0.0f)
        return 0;
    float h = layer->frame_display_h;
    uint32_t rows = (uint32_t)((h + layer->base.cell_size.height - 1.0f) /
                               layer->base.cell_size.height);
    if (rows == 0) rows = 1;
    return rows;
}

/* Anchor new frame at the current cursor; if it overflows the visible rows
 * below the cursor, ask the terminal to scroll. */
static void anchor_frame_and_fit(struct yetty_yterm_ymgui_layer *layer)
{
    /* rolling_row for top of the frame = cursor's rolling row. */
    layer->frame_rolling_row = layer->row0_absolute + layer->cursor_row;

    uint32_t span = frame_row_span(layer);
    uint32_t rows = layer->base.grid_size.rows;
    uint32_t room_below =
        (layer->cursor_row >= rows) ? 0u : (rows - layer->cursor_row);

    if (span > room_below && layer->base.scroll_fn && !layer->base.in_external_scroll) {
        int need = (int)(span - room_below);
        ydebug("ymgui: frame wants %u rows, %u fit under cursor — scroll %d",
               span, room_below, need);
        /* Ask every other layer (text, ypaint, …) to scroll up. Text layer
         * is what drives the PTY scroll and libvterm line reflow — once it
         * reports back via terminal_scroll_callback, our scroll() gets
         * called and bumps row0_absolute accordingly. */
        struct yetty_ycore_void_result sres = layer->base.scroll_fn(
            &layer->base, need, layer->base.scroll_userdata);
        if (YETTY_IS_ERR(sres)) {
            yerror("ymgui: scroll_fn failed: %s", sres.error.msg);
        }
    }
}

/*===========================================================================
 * Layer ops — forward declarations
 *=========================================================================*/

static void ymgui_destroy(struct yetty_yterm_terminal_layer *self);
static struct yetty_ycore_void_result
ymgui_write(struct yetty_yterm_terminal_layer *self, const char *data, size_t len);
static struct yetty_ycore_void_result
ymgui_resize_grid(struct yetty_yterm_terminal_layer *self, struct grid_size gs);
static struct yetty_ycore_void_result
ymgui_set_cell_size(struct yetty_yterm_terminal_layer *self,
                    struct pixel_size cs);
static struct yetty_ycore_void_result
ymgui_set_visual_zoom(struct yetty_yterm_terminal_layer *self,
                      float scale, float off_x, float off_y);
static struct yetty_yrender_gpu_resource_set_result
ymgui_get_gpu_resource_set(const struct yetty_yterm_terminal_layer *self);
static struct yetty_ycore_void_result
ymgui_render(struct yetty_yterm_terminal_layer *self,
             struct yetty_yrender_target *target);
static int  ymgui_is_empty(const struct yetty_yterm_terminal_layer *self);
static int  ymgui_on_key (struct yetty_yterm_terminal_layer *self, int key, int mods);
static int  ymgui_on_char(struct yetty_yterm_terminal_layer *self,
                          uint32_t cp, int mods);
static struct yetty_ycore_void_result
ymgui_scroll(struct yetty_yterm_terminal_layer *self, int lines);
static void ymgui_set_cursor(struct yetty_yterm_terminal_layer *self,
                             int col, int row);

static const struct yetty_yterm_terminal_layer_ops ymgui_ops = {
    .destroy              = ymgui_destroy,
    .write                = ymgui_write,
    .resize_grid          = ymgui_resize_grid,
    .set_cell_size        = ymgui_set_cell_size,
    .set_visual_zoom      = ymgui_set_visual_zoom,
    .get_gpu_resource_set = ymgui_get_gpu_resource_set,
    .render               = ymgui_render,
    .is_empty             = ymgui_is_empty,
    .on_key               = ymgui_on_key,
    .on_char              = ymgui_on_char,
    .scroll               = ymgui_scroll,
    .set_cursor           = ymgui_set_cursor,
};

/*===========================================================================
 * Create / destroy
 *=========================================================================*/

struct yetty_yterm_terminal_layer_result yetty_yterm_ymgui_layer_create(
    uint32_t cols, uint32_t rows, float cell_w, float cell_h,
    const struct yetty_context *context,
    yetty_yterm_request_render_fn request_render_fn,
    void *request_render_userdata,
    yetty_yterm_scroll_fn scroll_fn, void *scroll_userdata,
    yetty_yterm_cursor_fn cursor_fn, void *cursor_userdata)
{
    if (!context)
        return YETTY_ERR(yetty_yterm_terminal_layer, "context is NULL");

    /* Shader lives in paths/shaders just like the other layers. */
    struct yetty_yconfig *cfg = context->app_context.config;
    const char *shaders_dir = cfg->ops->get_string(cfg, "paths/shaders", "");
    char shader_path[512];
    snprintf(shader_path, sizeof(shader_path), "%s/ymgui-layer.wgsl", shaders_dir);

    struct yetty_ycore_buffer_result shader_res =
        yetty_ycore_read_file(shader_path);
    if (YETTY_IS_ERR(shader_res))
        return YETTY_ERR(yetty_yterm_terminal_layer, shader_res.error.msg);

    struct yetty_yterm_ymgui_layer *layer =
        calloc(1, sizeof(struct yetty_yterm_ymgui_layer));
    if (!layer) {
        free(shader_res.value.data);
        return YETTY_ERR(yetty_yterm_terminal_layer,
                         "failed to allocate ymgui layer");
    }

    layer->base.ops                       = &ymgui_ops;
    layer->base.grid_size.cols            = cols;
    layer->base.grid_size.rows            = rows;
    layer->base.cell_size.width           = cell_w;
    layer->base.cell_size.height          = cell_h;
    layer->base.dirty                     = 0;
    layer->base.pty_write_fn              = NULL;
    layer->base.pty_write_userdata        = NULL;
    layer->base.request_render_fn         = request_render_fn;
    layer->base.request_render_userdata   = request_render_userdata;
    layer->base.scroll_fn                 = scroll_fn;
    layer->base.scroll_userdata           = scroll_userdata;
    layer->base.cursor_fn                 = cursor_fn;
    layer->base.cursor_userdata           = cursor_userdata;

    layer->shader_code = shader_res.value;

    strncpy(layer->rs.namespace, "ymgui", YETTY_YRENDER_NAME_MAX - 1);

    /* One texture binding: the rasterised frame. Additional IMGUI textures
     * (font atlas, user images) are consumed on the CPU by the rasterizer,
     * so the GPU only ever sees a single pre-composited RGBA8. */
    layer->rs.texture_count = 1;
    strncpy(layer->rs.textures[0].name, "ymgui_raster",
            YETTY_YRENDER_NAME_MAX - 1);
    strncpy(layer->rs.textures[0].wgsl_type, "texture_2d<f32>",
            YETTY_YRENDER_WGSL_TYPE_MAX - 1);
    strncpy(layer->rs.textures[0].sampler_name, "ymgui_sampler",
            YETTY_YRENDER_NAME_MAX - 1);
    layer->rs.textures[0].format = 0; /* RGBA8 — actual WGPU enum picked by
                                       * the binder from wgsl_type + bpp */

    init_uniforms(&layer->rs);
    layer->rs.pixel_size.width  = (float)cols * cell_w;
    layer->rs.pixel_size.height = (float)rows * cell_h;
    layer->rs.uniforms[U_GRID_SIZE].vec2[0] = (float)cols;
    layer->rs.uniforms[U_GRID_SIZE].vec2[1] = (float)rows;
    layer->rs.uniforms[U_CELL_SIZE].vec2[0] = cell_w;
    layer->rs.uniforms[U_CELL_SIZE].vec2[1] = cell_h;

    yetty_yrender_shader_code_set(&layer->rs.shader,
                                  (const char *)layer->shader_code.data,
                                  layer->shader_code.size);

    /* Raster is allocated lazily on first --frame (sized to display_size).
     * Until then, bind a 1x1 transparent pixel so the WGPU pipeline has a
     * valid texture. */
    ymgui_raster_ensure(layer, 1, 1);
    ymgui_raster_clear(layer);

    ydebug("ymgui_layer_create: %ux%u grid, %.1fx%.1f cell",
           cols, rows, cell_w, cell_h);

    return YETTY_OK(yetty_yterm_terminal_layer, &layer->base);
}

static void ymgui_destroy(struct yetty_yterm_terminal_layer *self)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;
    if (!layer) return;
    free(layer->frame_bytes);
    free(layer->raster);
    tex_clear_all(layer);
    free(layer->shader_code.data);
    free(layer);
}

/*===========================================================================
 * write — OSC dispatch: --frame / --tex / --clear
 *=========================================================================*/

static struct yetty_ycore_void_result
handle_frame(struct yetty_yterm_ymgui_layer *layer,
             const uint8_t *raw, size_t raw_size)
{
    const struct ymgui_wire_frame *fh = NULL;
    if (validate_frame(raw, raw_size, &fh) != 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: malformed --frame payload");

    /* Copy payload — the OSC args buffer is transient. */
    uint8_t *copy = (uint8_t *)malloc(raw_size);
    if (!copy) return YETTY_ERR(yetty_ycore_void, "ymgui: oom");
    memcpy(copy, raw, raw_size);

    free(layer->frame_bytes);
    layer->frame_bytes      = copy;
    layer->frame_size       = raw_size;
    layer->has_frame        = 1;
    layer->frame_display_w  = fh->display_size_x;
    layer->frame_display_h  = fh->display_size_y;

    anchor_frame_and_fit(layer);

    ymgui_raster_frame(layer);
    layer->raster_dirty = 1;
    layer->base.dirty   = 1;

    if (layer->base.request_render_fn)
        layer->base.request_render_fn(layer->base.request_render_userdata);
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
handle_tex(struct yetty_yterm_ymgui_layer *layer,
           const uint8_t *raw, size_t raw_size)
{
    const struct ymgui_wire_tex *th = NULL;
    if (validate_tex(raw, raw_size, &th) != 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: malformed --tex payload");
    if (th->tex_id == 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: tex_id 0 is reserved");

    struct ymgui_tex_slot *slot = tex_find_or_alloc(layer, th->tex_id);
    if (!slot)
        return YETTY_ERR(yetty_ycore_void, "ymgui: texture slots exhausted");

    uint32_t bpp = (th->format == YMGUI_TEX_FMT_R8) ? 1u : 4u;
    size_t pbytes = (size_t)th->width * (size_t)th->height * bpp;

    uint8_t *px = (uint8_t *)malloc(pbytes ? pbytes : 1);
    if (!px) return YETTY_ERR(yetty_ycore_void, "ymgui: oom");
    memcpy(px, (const uint8_t *)(th + 1), pbytes);

    free(slot->pixels);
    slot->tex_id = th->tex_id;
    slot->format = th->format;
    slot->width  = th->width;
    slot->height = th->height;
    slot->pixels = px;

    ydebug("ymgui: tex uploaded id=%u %ux%u fmt=%u",
           th->tex_id, th->width, th->height, th->format);
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
ymgui_write(struct yetty_yterm_terminal_layer *self,
            const char *data, size_t len)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;
    struct yetty_yterm_osc_args args;
    struct yetty_ycore_void_result res = YETTY_OK_VOID();

    if (!data || len == 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: empty OSC body");

    if (yetty_yterm_osc_args_parse(&args, data, len) < 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: arg parse failed");

    if (yetty_yterm_osc_args_has(&args, "clear")) {
        free(layer->frame_bytes);
        layer->frame_bytes = NULL;
        layer->frame_size  = 0;
        layer->has_frame   = 0;
        ymgui_raster_clear(layer);
        layer->raster_dirty = 1;
        layer->base.dirty   = 1;
        if (layer->base.request_render_fn)
            layer->base.request_render_fn(layer->base.request_render_userdata);
        goto out;
    }

    /* --frame and --tex need a base64 payload */
    struct yetty_ycore_buffer_result payload =
        yetty_yterm_osc_args_get_payload_buffer(&args);
    if (YETTY_IS_ERR(payload)) {
        res = YETTY_ERR(yetty_ycore_void, "ymgui: missing payload");
        goto out;
    }

    uint8_t *raw = NULL;
    size_t raw_size = decode_b64_alloc((const char *)payload.value.data,
                                       payload.value.size, &raw);
    if (raw_size == (size_t)-1) {
        res = YETTY_ERR(yetty_ycore_void, "ymgui: base64 decode oom");
        goto out;
    }

    if (yetty_yterm_osc_args_has(&args, "frame")) {
        res = handle_frame(layer, raw, raw_size);
    } else if (yetty_yterm_osc_args_has(&args, "tex")) {
        res = handle_tex(layer, raw, raw_size);
    } else {
        res = YETTY_ERR(yetty_ycore_void,
                        "ymgui: expected --frame, --tex, or --clear");
    }

    free(raw);

out:
    yetty_yterm_osc_args_free(&args);
    return res;
}

/*===========================================================================
 * resize / set_cell_size / set_visual_zoom
 *=========================================================================*/

static struct yetty_ycore_void_result
ymgui_resize_grid(struct yetty_yterm_terminal_layer *self,
                  struct grid_size gs)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;

    self->grid_size = gs;
    layer->rs.uniforms[U_GRID_SIZE].vec2[0] = (float)gs.cols;
    layer->rs.uniforms[U_GRID_SIZE].vec2[1] = (float)gs.rows;
    layer->rs.pixel_size.width  = (float)gs.cols * self->cell_size.width;
    layer->rs.pixel_size.height = (float)gs.rows * self->cell_size.height;
    self->dirty = 1;
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
ymgui_set_cell_size(struct yetty_yterm_terminal_layer *self,
                    struct pixel_size cs)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;
    if (cs.width <= 0.0f || cs.height <= 0.0f)
        return YETTY_ERR(yetty_ycore_void, "ymgui: invalid cell size");

    self->cell_size = cs;
    layer->rs.uniforms[U_CELL_SIZE].vec2[0] = cs.width;
    layer->rs.uniforms[U_CELL_SIZE].vec2[1] = cs.height;
    layer->rs.pixel_size.width  = (float)self->grid_size.cols * cs.width;
    layer->rs.pixel_size.height = (float)self->grid_size.rows * cs.height;
    self->dirty = 1;
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
ymgui_set_visual_zoom(struct yetty_yterm_terminal_layer *self,
                      float scale, float off_x, float off_y)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;
    layer->rs.uniforms[U_VZ_SCALE].f32    = scale;
    layer->rs.uniforms[U_VZ_OFF].vec2[0]  = off_x;
    layer->rs.uniforms[U_VZ_OFF].vec2[1]  = off_y;
    self->dirty = 1;
    return YETTY_OK_VOID();
}

/*===========================================================================
 * get_gpu_resource_set — bind raster + current uniforms, shader does the rest
 *=========================================================================*/

static struct yetty_yrender_gpu_resource_set_result
ymgui_get_gpu_resource_set(const struct yetty_yterm_terminal_layer *self)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;

    /* Raster texture: pointer into owned buffer. */
    layer->rs.textures[0].data   = layer->raster;
    layer->rs.textures[0].width  = layer->raster_w;
    layer->rs.textures[0].height = layer->raster_h;
    layer->rs.textures[0].dirty  = layer->raster_dirty;
    layer->raster_dirty = 0;

    /* Scroll state. Visibility test: frame_rolling_row must lie in
     * [row0, row0 + grid_rows). If not, hide. */
    uint32_t row0 = layer->row0_absolute;
    uint32_t rows = layer->base.grid_size.rows;
    int visible =
        layer->has_frame &&
        layer->frame_rolling_row + frame_row_span(layer) > row0 &&
        layer->frame_rolling_row < row0 + rows;

    layer->rs.uniforms[U_ROW_ORIGIN].u32    = row0;
    layer->rs.uniforms[U_FRAME_ROLLING].u32 = layer->frame_rolling_row;
    layer->rs.uniforms[U_FRAME_SIZE].vec2[0] = layer->frame_display_w;
    layer->rs.uniforms[U_FRAME_SIZE].vec2[1] = layer->frame_display_h;
    layer->rs.uniforms[U_FRAME_PRESENT].u32 = visible ? 1u : 0u;

    return YETTY_OK(yetty_yrender_gpu_resource_set, &layer->rs);
}

/*===========================================================================
 * render — single fullscreen-quad pass, same shape as ypaint-layer
 *=========================================================================*/

static struct yetty_ycore_void_result
ymgui_render(struct yetty_yterm_terminal_layer *self,
             struct yetty_yrender_target *target)
{
    return target->ops->render_layer(target, self);
}

/*===========================================================================
 * is_empty / input
 *=========================================================================*/

static int ymgui_is_empty(const struct yetty_yterm_terminal_layer *self)
{
    const struct yetty_yterm_ymgui_layer *layer =
        (const struct yetty_yterm_ymgui_layer *)self;
    if (!layer->has_frame) return 1;
    uint32_t span = frame_row_span((struct yetty_yterm_ymgui_layer *)layer);
    /* Anchor fully scrolled off the top? */
    if (layer->frame_rolling_row + span <= layer->row0_absolute) return 1;
    /* Anchor below the visible window? (can happen if cursor moved up) */
    if (layer->frame_rolling_row >=
        layer->row0_absolute + layer->base.grid_size.rows) return 1;
    return 0;
}

/* Input: ImGui input is handled by a separate platform backend on the client
 * (mouse via DEC 1500/1501). The layer itself does not consume key/char. */
static int ymgui_on_key (struct yetty_yterm_terminal_layer *self, int key, int mods)
{ (void)self; (void)key; (void)mods; return 0; }

static int ymgui_on_char(struct yetty_yterm_terminal_layer *self,
                         uint32_t cp, int mods)
{ (void)self; (void)cp; (void)mods; return 0; }

/*===========================================================================
 * scroll / set_cursor — fan-in from other layers
 *=========================================================================*/

static struct yetty_ycore_void_result
ymgui_scroll(struct yetty_yterm_terminal_layer *self, int lines)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;
    if (lines <= 0) return YETTY_OK_VOID();

    /* O(1): bump row0. Anchor rolling_row stays put, so the frame visually
     * scrolls up with the text. */
    layer->row0_absolute += (uint32_t)lines;
    layer->base.dirty = 1;
    return YETTY_OK_VOID();
}

static void ymgui_set_cursor(struct yetty_yterm_terminal_layer *self,
                             int col, int row)
{
    struct yetty_yterm_ymgui_layer *layer =
        (struct yetty_yterm_ymgui_layer *)self;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    layer->cursor_col = (uint32_t)col;
    layer->cursor_row = (uint32_t)row;
    /* Next --frame will anchor at this cell. */
}

/*===========================================================================
 * CPU rasterizer
 * ---------------------------------------------------------------------------
 * Walks ymgui_wire_frame -> cmd_lists -> triangles. For each cmd, scissors
 * to its clip rect, iterates triangles, and shades per-pixel:
 *   sample = (tex_id == FONT_ATLAS) ? atlas_r8_at(uv) : 1.0
 *   color  = vtx_color * vec4(1,1,1, sample)
 * Alpha-blends into the RGBA8 raster.
 *
 * This is intentionally simple (no SIMD, no multisample). It is correct for
 * solid rects, alpha-textured fonts, and most ImGui widgets. Tight loops are
 * the obvious optimisation target if needed.
 *=========================================================================*/

static void ymgui_raster_ensure(struct yetty_yterm_ymgui_layer *layer,
                                uint32_t w, uint32_t h)
{
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (layer->raster && layer->raster_w == w && layer->raster_h == h)
        return;
    free(layer->raster);
    layer->raster = (uint8_t *)calloc((size_t)w * h * 4u, 1);
    layer->raster_w = w;
    layer->raster_h = h;
}

static void ymgui_raster_clear(struct yetty_yterm_ymgui_layer *layer)
{
    if (!layer->raster) return;
    memset(layer->raster, 0, (size_t)layer->raster_w * layer->raster_h * 4u);
}

/* IM_COL32 = AABBGGRR (little-endian). Return R,G,B,A as floats in [0..1]. */
static inline void unpack_col(uint32_t c, float *r, float *g, float *b, float *a)
{
    *r = (float)( c        & 0xFFu) / 255.0f;
    *g = (float)((c >> 8)  & 0xFFu) / 255.0f;
    *b = (float)((c >> 16) & 0xFFu) / 255.0f;
    *a = (float)((c >> 24) & 0xFFu) / 255.0f;
}

static inline void blend_pixel(uint8_t *dst, float r, float g, float b, float a)
{
    /* Straight-alpha blend: dst = src*a + dst*(1-a). */
    float inv = 1.0f - a;
    dst[0] = (uint8_t)f_clamp(r * 255.0f * a + (float)dst[0] * inv, 0.0f, 255.0f);
    dst[1] = (uint8_t)f_clamp(g * 255.0f * a + (float)dst[1] * inv, 0.0f, 255.0f);
    dst[2] = (uint8_t)f_clamp(b * 255.0f * a + (float)dst[2] * inv, 0.0f, 255.0f);
    /* Alpha: src-over. */
    dst[3] = (uint8_t)f_clamp((float)dst[3] * inv + 255.0f * a, 0.0f, 255.0f);
}

static inline float edge(float ax, float ay, float bx, float by,
                         float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

/* Sample R8 atlas with bilinear filter, return coverage in [0..1]. */
static float sample_r8(const struct ymgui_tex_slot *s, float u, float v)
{
    if (!s || !s->pixels || s->width == 0 || s->height == 0) return 1.0f;
    float x = u * (float)s->width  - 0.5f;
    float y = v * (float)s->height - 0.5f;
    int x0 = (int)f_max(0.0f, f_min((float)(s->width - 1),  x));
    int y0 = (int)f_max(0.0f, f_min((float)(s->height - 1), y));
    /* Nearest is adequate for small bitmap fonts; keep it simple. */
    return (float)s->pixels[(size_t)y0 * s->width + x0] / 255.0f;
}

static float sample_rgba8_a(const struct ymgui_tex_slot *s, float u, float v,
                            float *out_r, float *out_g, float *out_b)
{
    if (!s || !s->pixels || s->width == 0 || s->height == 0) {
        *out_r = *out_g = *out_b = 1.0f;
        return 1.0f;
    }
    int x = (int)f_clamp(u * (float)s->width,  0.0f, (float)(s->width  - 1));
    int y = (int)f_clamp(v * (float)s->height, 0.0f, (float)(s->height - 1));
    const uint8_t *p = s->pixels + ((size_t)y * s->width + x) * 4u;
    *out_r = (float)p[0] / 255.0f;
    *out_g = (float)p[1] / 255.0f;
    *out_b = (float)p[2] / 255.0f;
    return   (float)p[3] / 255.0f;
}

/* Draw one triangle. Clips to [cx0,cx1) x [cy0,cy1). Vertex positions are
 * shifted by (-tx,-ty) so the caller can pass DisplayPos to get frame-local
 * coordinates. */
static void raster_triangle(struct yetty_yterm_ymgui_layer *layer,
                            const struct ymgui_wire_vertex *v0,
                            const struct ymgui_wire_vertex *v1,
                            const struct ymgui_wire_vertex *v2,
                            const struct ymgui_tex_slot *tex,
                            int cx0, int cy0, int cx1, int cy1,
                            float tx, float ty)
{
    float p0x = v0->pos_x - tx, p0y = v0->pos_y - ty;
    float p1x = v1->pos_x - tx, p1y = v1->pos_y - ty;
    float p2x = v2->pos_x - tx, p2y = v2->pos_y - ty;

    float minx = f_min(f_min(p0x, p1x), p2x);
    float miny = f_min(f_min(p0y, p1y), p2y);
    float maxx = f_max(f_max(p0x, p1x), p2x);
    float maxy = f_max(f_max(p0y, p1y), p2y);

    int x0 = (int)f_max((float)cx0, minx);
    int y0 = (int)f_max((float)cy0, miny);
    int x1 = (int)f_min((float)cx1, maxx + 1.0f);
    int y1 = (int)f_min((float)cy1, maxy + 1.0f);
    if (x1 <= x0 || y1 <= y0) return;

    float area = edge(p0x, p0y, p1x, p1y, p2x, p2y);
    if (area == 0.0f) return;
    float inv_area = 1.0f / area;

    float r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2;
    unpack_col(v0->col, &r0, &g0, &b0, &a0);
    unpack_col(v1->col, &r1, &g1, &b1, &a1);
    unpack_col(v2->col, &r2, &g2, &b2, &a2);

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = edge(p1x, p1y, p2x, p2y, px, py);
            float w1 = edge(p2x, p2y, p0x, p0y, px, py);
            float w2 = edge(p0x, p0y, p1x, p1y, px, py);

            int inside = (area > 0.0f)
                ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
            if (!inside) continue;

            float bw0 = w0 * inv_area;
            float bw1 = w1 * inv_area;
            float bw2 = w2 * inv_area;

            float r = r0 * bw0 + r1 * bw1 + r2 * bw2;
            float g = g0 * bw0 + g1 * bw1 + g2 * bw2;
            float b = b0 * bw0 + b1 * bw1 + b2 * bw2;
            float a = a0 * bw0 + a1 * bw1 + a2 * bw2;

            if (tex) {
                float u = v0->uv_x * bw0 + v1->uv_x * bw1 + v2->uv_x * bw2;
                float v = v0->uv_y * bw0 + v1->uv_y * bw1 + v2->uv_y * bw2;
                if (tex->format == YMGUI_TEX_FMT_R8) {
                    a *= sample_r8(tex, u, v);
                } else {
                    float tr, tg, tb;
                    float ta = sample_rgba8_a(tex, u, v, &tr, &tg, &tb);
                    r *= tr; g *= tg; b *= tb; a *= ta;
                }
            }
            if (a <= 0.0f) continue;

            uint8_t *dst = layer->raster + ((size_t)y * layer->raster_w + x) * 4u;
            blend_pixel(dst, r, g, b, a);
        }
    }
}

static void ymgui_raster_frame(struct yetty_yterm_ymgui_layer *layer)
{
    if (!layer->has_frame || !layer->frame_bytes) {
        ymgui_raster_clear(layer);
        return;
    }

    const struct ymgui_wire_frame *fh =
        (const struct ymgui_wire_frame *)layer->frame_bytes;
    const uint8_t *cur = layer->frame_bytes + sizeof(*fh);
    const uint8_t *end = layer->frame_bytes + layer->frame_size;
    int idx32 = (fh->flags & YMGUI_FRAME_FLAG_IDX32) ? 1 : 0;

    /* Size raster exactly to the frame. Positions in ImDrawVert are in the
     * frame's own coordinate system — DisplayPos subtracted upfront so (0,0)
     * is the frame's top-left. */
    uint32_t fw = (uint32_t)f_max(1.0f, fh->display_size_x);
    uint32_t fh_px = (uint32_t)f_max(1.0f, fh->display_size_y);
    ymgui_raster_ensure(layer, fw, fh_px);
    ymgui_raster_clear(layer);

    int raster_w = (int)layer->raster_w;
    int raster_h = (int)layer->raster_h;

    float dp_x = fh->display_pos_x;
    float dp_y = fh->display_pos_y;

    for (uint32_t li = 0; li < fh->cmd_list_count; li++) {
        if (cur + sizeof(struct ymgui_wire_cmd_list) > end) return;
        const struct ymgui_wire_cmd_list *clh =
            (const struct ymgui_wire_cmd_list *)cur;
        cur += sizeof(*clh);

        const struct ymgui_wire_vertex *vtx =
            (const struct ymgui_wire_vertex *)cur;
        cur += (size_t)clh->vtx_count * sizeof(struct ymgui_wire_vertex);
        if (cur > end) return;

        const uint8_t *idx_bytes = cur;
        size_t idx_bpe = idx32 ? 4u : 2u;
        cur += (size_t)clh->idx_count * idx_bpe;
        /* Pad to 4 (wire.h contract). */
        size_t idx_total = (size_t)clh->idx_count * idx_bpe;
        if (idx_total & 3u) cur += 4u - (idx_total & 3u);
        if (cur > end) return;

        const struct ymgui_wire_cmd *cmds =
            (const struct ymgui_wire_cmd *)cur;
        cur += (size_t)clh->cmd_count * sizeof(struct ymgui_wire_cmd);
        if (cur > end) return;

        for (uint32_t ci = 0; ci < clh->cmd_count; ci++) {
            const struct ymgui_wire_cmd *c = &cmds[ci];
            const struct ymgui_tex_slot *tex = tex_find(layer, c->tex_id);

            /* Clip rect is in DisplayPos-relative coords in ImGui's model —
             * shift like the verts. */
            int scx0 = (int)f_max(0.0f, c->clip_min_x - dp_x);
            int scy0 = (int)f_max(0.0f, c->clip_min_y - dp_y);
            int scx1 = (int)f_min((float)raster_w, c->clip_max_x - dp_x);
            int scy1 = (int)f_min((float)raster_h, c->clip_max_y - dp_y);
            if (scx1 <= scx0 || scy1 <= scy0) continue;

            for (uint32_t t = 0; t < c->elem_count; t += 3u) {
                uint32_t i0, i1, i2;
                if (idx32) {
                    const uint32_t *p =
                        (const uint32_t *)idx_bytes + c->idx_offset + t;
                    i0 = p[0]; i1 = p[1]; i2 = p[2];
                } else {
                    const uint16_t *p =
                        (const uint16_t *)idx_bytes + c->idx_offset + t;
                    i0 = p[0]; i1 = p[1]; i2 = p[2];
                }
                i0 += c->vtx_offset;
                i1 += c->vtx_offset;
                i2 += c->vtx_offset;
                if (i0 >= clh->vtx_count || i1 >= clh->vtx_count ||
                    i2 >= clh->vtx_count)
                    continue;

                raster_triangle(layer, &vtx[i0], &vtx[i1], &vtx[i2],
                                tex, scx0, scy0, scx1, scy1, dp_x, dp_y);
            }
        }
    }
}
