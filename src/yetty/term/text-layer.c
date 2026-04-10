#include <yetty/term/text-layer.h>
#include <yetty/font/font.h>
#include <yetty/font/raster-font.h>
#include <yetty/render/gpu-resource-set.h>
#include <yetty/config.h>
#include <yetty/core/types.h>
#include <yetty/ytrace.h>
#include <vterm.h>
#include <stdlib.h>
#include <string.h>

#define INCBIN_STYLE 1
#include <incbin.h>

/* Embedded shader code */
INCBIN(text_layer_shader, TEXT_LAYER_SHADER_PATH);

/* Uniform positions */
#define U_GRID_SIZE       0
#define U_CELL_SIZE       1
#define U_CURSOR_POS      2
#define U_CURSOR_VISIBLE  3
#define U_CURSOR_SHAPE    4
#define U_SCALE           5
#define U_DEFAULT_FG      6
#define U_DEFAULT_BG      7
#define U_COUNT           8

/* Setters */
static inline void set_grid_size(struct yetty_render_gpu_resource_set *rs, float cols, float rows) {
    rs->uniforms[U_GRID_SIZE].vec2[0] = cols;
    rs->uniforms[U_GRID_SIZE].vec2[1] = rows;
}
static inline void set_cell_size(struct yetty_render_gpu_resource_set *rs, float w, float h) {
    rs->uniforms[U_CELL_SIZE].vec2[0] = w;
    rs->uniforms[U_CELL_SIZE].vec2[1] = h;
}
static inline void set_cursor_pos(struct yetty_render_gpu_resource_set *rs, float col, float row) {
    rs->uniforms[U_CURSOR_POS].vec2[0] = col;
    rs->uniforms[U_CURSOR_POS].vec2[1] = row;
}
static inline void set_cursor_visible(struct yetty_render_gpu_resource_set *rs, float v) {
    rs->uniforms[U_CURSOR_VISIBLE].f32 = v;
}
static inline void set_cursor_shape(struct yetty_render_gpu_resource_set *rs, float s) {
    rs->uniforms[U_CURSOR_SHAPE].f32 = s;
}
static inline void set_scale(struct yetty_render_gpu_resource_set *rs, float s) {
    rs->uniforms[U_SCALE].f32 = s;
}
static inline void set_default_fg(struct yetty_render_gpu_resource_set *rs, uint32_t c) {
    rs->uniforms[U_DEFAULT_FG].u32 = c;
}
static inline void set_default_bg(struct yetty_render_gpu_resource_set *rs, uint32_t c) {
    rs->uniforms[U_DEFAULT_BG].u32 = c;
}

/* Init — names and types use the same constants */
static void init_uniforms(struct yetty_render_gpu_resource_set *rs)
{
    rs->uniform_count = U_COUNT;

    rs->uniforms[U_GRID_SIZE]      = (struct yetty_render_uniform){"grid_size",      YETTY_RENDER_UNIFORM_VEC2};
    rs->uniforms[U_CELL_SIZE]      = (struct yetty_render_uniform){"cell_size",      YETTY_RENDER_UNIFORM_VEC2};
    rs->uniforms[U_CURSOR_POS]     = (struct yetty_render_uniform){"cursor_pos",     YETTY_RENDER_UNIFORM_VEC2};
    rs->uniforms[U_CURSOR_VISIBLE] = (struct yetty_render_uniform){"cursor_visible", YETTY_RENDER_UNIFORM_F32};
    rs->uniforms[U_CURSOR_SHAPE]   = (struct yetty_render_uniform){"cursor_shape",   YETTY_RENDER_UNIFORM_F32};
    rs->uniforms[U_SCALE]          = (struct yetty_render_uniform){"scale",          YETTY_RENDER_UNIFORM_F32};
    rs->uniforms[U_DEFAULT_FG]     = (struct yetty_render_uniform){"default_fg",     YETTY_RENDER_UNIFORM_U32};
    rs->uniforms[U_DEFAULT_BG]     = (struct yetty_render_uniform){"default_bg",     YETTY_RENDER_UNIFORM_U32};

    set_scale(rs, 1.0f);
    set_cursor_shape(rs, 1.0f);
    set_default_fg(rs, 0x00FFFFFFu);
    set_default_bg(rs, 0x00000000u);
}

/* Text layer - embeds base as first member */
struct yetty_term_terminal_text_layer {
    struct yetty_term_terminal_layer base;
    VTerm *vterm;
    VTermScreen *screen;
    struct yetty_font_font *font;  /* not owned */
    struct yetty_render_gpu_resource_set rs;
};

/* Forward declarations */
static void text_layer_destroy(struct yetty_term_terminal_layer *self);
static void text_layer_write(struct yetty_term_terminal_layer *self,
                             const char *data, size_t len);
static void text_layer_resize(struct yetty_term_terminal_layer *self,
                              uint32_t cols, uint32_t rows);
static struct yetty_render_gpu_resource_set_result text_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self);

/* VTerm callbacks */
static int on_damage(VTermRect rect, void *user);
static int on_move_cursor(VTermPos pos, VTermPos oldpos, int visible, void *user);

/* Ops */
static const struct yetty_term_terminal_layer_ops text_layer_ops = {
    .destroy = text_layer_destroy,
    .write = text_layer_write,
    .resize = text_layer_resize,
    .get_gpu_resource_set = text_layer_get_gpu_resource_set,
};

/* VTerm screen callbacks */
static VTermScreenCallbacks screen_callbacks = {
    .damage = on_damage,
    .moverect = NULL,
    .movecursor = on_move_cursor,
    .settermprop = NULL,
    .bell = NULL,
    .resize = NULL,
    .sb_pushline = NULL,
    .sb_popline = NULL,
    .sb_clear = NULL,
};

/* Create */

struct yetty_term_terminal_layer_result yetty_term_terminal_text_layer_create(
    uint32_t cols, uint32_t rows,
    const struct yetty_context *context)
{
    struct yetty_term_terminal_text_layer *text_layer;

    text_layer = calloc(1, sizeof(struct yetty_term_terminal_text_layer));
    if (!text_layer)
        return YETTY_ERR(yetty_term_terminal_layer, "failed to allocate text layer");

    text_layer->base.ops = &text_layer_ops;
    text_layer->base.cols = cols;
    text_layer->base.rows = rows;
    text_layer->base.cell_width = 10.0f;
    text_layer->base.cell_height = 20.0f;
    text_layer->base.dirty = 1;

    /* Create font from config */
    struct yetty_font_font_result font_res = yetty_font_raster_font_create(
        context->app_context.config, text_layer->base.cell_width, text_layer->base.cell_height);
    if (!YETTY_IS_OK(font_res)) {
        free(text_layer);
        return YETTY_ERR(yetty_term_terminal_layer, font_res.error.msg);
    }
    text_layer->font = font_res.value;

    text_layer->vterm = vterm_new((int)rows, (int)cols);
    if (!text_layer->vterm) {
        free(text_layer);
        return YETTY_ERR(yetty_term_terminal_layer, "failed to create vterm");
    }

    vterm_set_utf8(text_layer->vterm, 1);
    text_layer->screen = vterm_obtain_screen(text_layer->vterm, NULL, NULL);
    vterm_screen_set_callbacks(text_layer->screen, &screen_callbacks, text_layer);
    vterm_screen_enable_altscreen(text_layer->screen, 1);
    vterm_screen_enable_reflow(text_layer->screen, 1);
    vterm_screen_reset(text_layer->screen, 1);

    /* Resource set */
    strncpy(text_layer->rs.namespace, "text_grid", YETTY_RENDER_NAME_MAX - 1);

    text_layer->rs.buffer_count = 1;
    strncpy(text_layer->rs.buffers[0].name, "buffer", YETTY_RENDER_NAME_MAX - 1);
    strncpy(text_layer->rs.buffers[0].wgsl_type, "array<u32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
    text_layer->rs.buffers[0].readonly = 1;

    init_uniforms(&text_layer->rs);
    set_grid_size(&text_layer->rs, (float)cols, (float)rows);
    set_cell_size(&text_layer->rs, text_layer->base.cell_width, text_layer->base.cell_height);

    text_layer->rs.shader_code = (const char *)gtext_layer_shader_data;
    text_layer->rs.shader_code_size = gtext_layer_shader_size;

    if (text_layer->font)
        text_layer->rs.children_count = 1;

    return YETTY_OK(yetty_term_terminal_layer, &text_layer->base);
}

/* Ops implementations */

static void text_layer_destroy(struct yetty_term_terminal_layer *self)
{
    struct yetty_term_terminal_text_layer *text_layer =
        container_of(self, struct yetty_term_terminal_text_layer, base);

    if (text_layer->vterm)
        vterm_free(text_layer->vterm);

    free(text_layer);
}

static void text_layer_write(struct yetty_term_terminal_layer *self,
                             const char *data, size_t len)
{
    struct yetty_term_terminal_text_layer *text_layer =
        container_of(self, struct yetty_term_terminal_text_layer, base);

    if (text_layer->vterm && len > 0)
        vterm_input_write(text_layer->vterm, data, len);
}

static void text_layer_resize(struct yetty_term_terminal_layer *self,
                              uint32_t cols, uint32_t rows)
{
    struct yetty_term_terminal_text_layer *text_layer =
        container_of(self, struct yetty_term_terminal_text_layer, base);

    if (text_layer->vterm) {
        vterm_set_size(text_layer->vterm, (int)rows, (int)cols);
        self->cols = cols;
        self->rows = rows;
        set_grid_size(&text_layer->rs, (float)cols, (float)rows);
    }
}

static struct yetty_render_gpu_resource_set_result text_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self)
{
    struct yetty_term_terminal_text_layer *text_layer =
        (struct yetty_term_terminal_text_layer *)
        ((const char *)self - offsetof(struct yetty_term_terminal_text_layer, base));

    /* Update cell buffer pointer to current vterm screen data */
    const VTermScreenCell *cells = vterm_screen_get_buffer(text_layer->screen);
    text_layer->rs.buffers[0].data = (uint8_t *)cells;
    text_layer->rs.buffers[0].size = self->cols * self->rows * sizeof(VTermScreenCell);

    /* Update font child pointer */
    if (text_layer->font && text_layer->font->ops &&
        text_layer->font->ops->get_gpu_resource_set) {
        struct yetty_render_gpu_resource_set_result font_rs =
            text_layer->font->ops->get_gpu_resource_set(text_layer->font);
        if (YETTY_IS_OK(font_rs))
            text_layer->rs.children[0] = (struct yetty_render_gpu_resource_set *)font_rs.value;
    }

    return YETTY_OK(yetty_render_gpu_resource_set, &text_layer->rs);
}

/* VTerm callbacks */

static int on_damage(VTermRect rect, void *user)
{
    struct yetty_term_terminal_text_layer *text_layer = user;
    (void)rect;
    text_layer->base.dirty = 1;
    return 1;
}

static int on_move_cursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    struct yetty_term_terminal_text_layer *text_layer = user;
    (void)oldpos;
    set_cursor_pos(&text_layer->rs, (float)pos.col, (float)pos.row);
    set_cursor_visible(&text_layer->rs, visible ? 1.0f : 0.0f);
    text_layer->base.dirty = 1;
    return 1;
}
