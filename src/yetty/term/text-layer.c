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
static int text_layer_on_key(struct yetty_term_terminal_layer *self, int key, int mods);
static int text_layer_on_char(struct yetty_term_terminal_layer *self, uint32_t codepoint, int mods);

/* VTerm callbacks */
static int on_damage(VTermRect rect, void *user);
static int on_move_cursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
static int on_sb_pushline(int cols, const VTermScreenCell *cells, void *user);

/* Glyph resolver — called by vterm for every codepoint */
static VTermResolvedGlyph resolve_glyph(const uint32_t *chars, int count,
                                         int bold, int italic, void *user)
{
    struct yetty_term_terminal_text_layer *text_layer = user;
    VTermResolvedGlyph result = {0, 0};

    if (!text_layer->font || !text_layer->font->ops || count == 0)
        return result;

    enum yetty_font_style style = YETTY_FONT_STYLE_REGULAR;
    if (bold && italic)      style = YETTY_FONT_STYLE_BOLD_ITALIC;
    else if (bold)           style = YETTY_FONT_STYLE_BOLD;
    else if (italic)         style = YETTY_FONT_STYLE_ITALIC;

    result.glyph_index = text_layer->font->ops->get_glyph_index_styled(
        text_layer->font, chars[0], style);
    result.font_type = YETTY_FONT_RENDER_METHOD_RASTER;

    return result;
}

/* Text layer always has content */
static int text_layer_is_empty(const struct yetty_term_terminal_layer *self)
{
    (void)self;
    return 0;
}

/* Ops */
static const struct yetty_term_terminal_layer_ops text_layer_ops = {
    .destroy = text_layer_destroy,
    .write = text_layer_write,
    .resize = text_layer_resize,
    .get_gpu_resource_set = text_layer_get_gpu_resource_set,
    .is_empty = text_layer_is_empty,
    .on_key = text_layer_on_key,
    .on_char = text_layer_on_char,
    .scroll = NULL,  /* text layer is scroll source, doesn't receive scroll */
};

/* VTerm screen callbacks */
static VTermScreenCallbacks screen_callbacks = {
    .damage = on_damage,
    .moverect = NULL,
    .movecursor = on_move_cursor,
    .settermprop = NULL,
    .bell = NULL,
    .resize = NULL,
    .sb_pushline = on_sb_pushline,
    .sb_popline = NULL,
    .sb_clear = NULL,
};

/* Create */

/* VTerm output callback - forwards to layer's PTY write callback */
static void vterm_output_callback(const char *data, size_t len, void *user)
{
    struct yetty_term_terminal_text_layer *text_layer = user;
    if (text_layer->base.pty_write_fn) {
        text_layer->base.pty_write_fn(data, len, text_layer->base.pty_write_userdata);
    }
}

struct yetty_term_terminal_layer_result yetty_term_terminal_text_layer_create(
    uint32_t cols, uint32_t rows,
    const struct yetty_context *context,
    yetty_term_pty_write_fn pty_write_fn,
    void *pty_write_userdata,
    yetty_term_request_render_fn request_render_fn,
    void *request_render_userdata,
    yetty_term_scroll_fn scroll_fn,
    void *scroll_userdata,
    yetty_term_cursor_fn cursor_fn,
    void *cursor_userdata)
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
    text_layer->base.pty_write_fn = pty_write_fn;
    text_layer->base.pty_write_userdata = pty_write_userdata;
    text_layer->base.request_render_fn = request_render_fn;
    text_layer->base.request_render_userdata = request_render_userdata;
    text_layer->base.scroll_fn = scroll_fn;
    text_layer->base.scroll_userdata = scroll_userdata;
    text_layer->base.cursor_fn = cursor_fn;
    text_layer->base.cursor_userdata = cursor_userdata;

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
    text_layer->screen = vterm_obtain_screen(text_layer->vterm, resolve_glyph, text_layer);
    vterm_screen_set_callbacks(text_layer->screen, &screen_callbacks, text_layer);
    vterm_screen_enable_altscreen(text_layer->screen, 1);
    vterm_screen_enable_reflow(text_layer->screen, 1);
    vterm_screen_reset(text_layer->screen, 1);

    /* Set up vterm output callback to write to PTY */
    vterm_output_set_callback(text_layer->vterm, vterm_output_callback, text_layer);

    /* Resource set */
    strncpy(text_layer->rs.namespace, "text_grid", YETTY_RENDER_NAME_MAX - 1);

    text_layer->rs.buffer_count = 1;
    strncpy(text_layer->rs.buffers[0].name, "buffer", YETTY_RENDER_NAME_MAX - 1);
    strncpy(text_layer->rs.buffers[0].wgsl_type, "array<u32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
    text_layer->rs.buffers[0].readonly = 1;

    init_uniforms(&text_layer->rs);
    set_grid_size(&text_layer->rs, (float)cols, (float)rows);
    set_cell_size(&text_layer->rs, text_layer->base.cell_width, text_layer->base.cell_height);

    yetty_render_shader_code_set(&text_layer->rs.shader,
        (const char *)gtext_layer_shader_data, gtext_layer_shader_size);

    if (text_layer->font)
        text_layer->rs.children_count = 1;

    /* Initial buffer setup - point to vterm buffer directly.
     * Cast away const is safe: buffer is readonly, GPU only reads from it. */
    text_layer->rs.buffers[0].data = (uint8_t *)vterm_screen_get_buffer(text_layer->screen);
    text_layer->rs.buffers[0].size = vterm_screen_get_buffer_size(text_layer->screen);
    text_layer->rs.buffers[0].readonly = 1;

    /* Clear dirty — vterm_screen_reset fires on_damage but there's no real content yet.
     * First real dirty will come from PTY data via on_damage. */
    text_layer->base.dirty = 0;
    text_layer->rs.buffers[0].dirty = 0;

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
        /* Buffer pointer updated in get_gpu_resource_set when dirty */
        self->dirty = 1;
    }
}

/* Convert GLFW modifier flags to VTerm modifier flags */
static VTermModifier glfw_mods_to_vterm(int mods)
{
    VTermModifier vt_mod = VTERM_MOD_NONE;
    if (mods & 0x0001) vt_mod |= VTERM_MOD_SHIFT;
    if (mods & 0x0002) vt_mod |= VTERM_MOD_CTRL;
    if (mods & 0x0004) vt_mod |= VTERM_MOD_ALT;
    return vt_mod;
}

/* Convert GLFW key code to VTerm key (for special keys) */
static VTermKey glfw_key_to_vterm(int key)
{
    switch (key) {
    case 257: return VTERM_KEY_ENTER;      /* GLFW_KEY_ENTER */
    case 258: return VTERM_KEY_TAB;        /* GLFW_KEY_TAB */
    case 259: return VTERM_KEY_BACKSPACE;  /* GLFW_KEY_BACKSPACE */
    case 260: return VTERM_KEY_INS;        /* GLFW_KEY_INSERT */
    case 261: return VTERM_KEY_DEL;        /* GLFW_KEY_DELETE */
    case 262: return VTERM_KEY_RIGHT;      /* GLFW_KEY_RIGHT */
    case 263: return VTERM_KEY_LEFT;       /* GLFW_KEY_LEFT */
    case 264: return VTERM_KEY_DOWN;       /* GLFW_KEY_DOWN */
    case 265: return VTERM_KEY_UP;         /* GLFW_KEY_UP */
    case 266: return VTERM_KEY_PAGEUP;     /* GLFW_KEY_PAGE_UP */
    case 267: return VTERM_KEY_PAGEDOWN;   /* GLFW_KEY_PAGE_DOWN */
    case 268: return VTERM_KEY_HOME;       /* GLFW_KEY_HOME */
    case 269: return VTERM_KEY_END;        /* GLFW_KEY_END */
    case 256: return VTERM_KEY_ESCAPE;     /* GLFW_KEY_ESCAPE */
    case 290: return VTERM_KEY_FUNCTION(1);
    case 291: return VTERM_KEY_FUNCTION(2);
    case 292: return VTERM_KEY_FUNCTION(3);
    case 293: return VTERM_KEY_FUNCTION(4);
    case 294: return VTERM_KEY_FUNCTION(5);
    case 295: return VTERM_KEY_FUNCTION(6);
    case 296: return VTERM_KEY_FUNCTION(7);
    case 297: return VTERM_KEY_FUNCTION(8);
    case 298: return VTERM_KEY_FUNCTION(9);
    case 299: return VTERM_KEY_FUNCTION(10);
    case 300: return VTERM_KEY_FUNCTION(11);
    case 301: return VTERM_KEY_FUNCTION(12);
    default:  return VTERM_KEY_NONE;
    }
}

static int text_layer_on_key(struct yetty_term_terminal_layer *self, int key, int mods)
{
    struct yetty_term_terminal_text_layer *text_layer =
        container_of(self, struct yetty_term_terminal_text_layer, base);

    if (!text_layer->vterm)
        return 0;

    VTermKey vt_key = glfw_key_to_vterm(key);
    if (vt_key != VTERM_KEY_NONE) {
        VTermModifier vt_mod = glfw_mods_to_vterm(mods);
        vterm_keyboard_key(text_layer->vterm, vt_key, vt_mod);
        ydebug("text_layer_on_key: key=%d vt_key=%d mods=%d", key, (int)vt_key, mods);
        return 1;
    }
    return 0;  /* Not a special key */
}

static int text_layer_on_char(struct yetty_term_terminal_layer *self, uint32_t codepoint, int mods)
{
    struct yetty_term_terminal_text_layer *text_layer =
        container_of(self, struct yetty_term_terminal_text_layer, base);

    if (!text_layer->vterm)
        return 0;

    VTermModifier vt_mod = glfw_mods_to_vterm(mods);
    vterm_keyboard_unichar(text_layer->vterm, codepoint, vt_mod);
    ydebug("text_layer_on_char: codepoint=U+%04X mods=%d", codepoint, mods);
    return 1;
}

static struct yetty_render_gpu_resource_set_result text_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self)
{
    struct yetty_term_terminal_text_layer *text_layer =
        (struct yetty_term_terminal_text_layer *)
        ((const char *)self - offsetof(struct yetty_term_terminal_text_layer, base));

    /* Use vterm buffer directly - no conversion needed.
     * VTermScreenCell is already 12 bytes matching shader expectations.
     * Cast away const is safe: buffer is readonly, GPU only reads from it. */
    if (text_layer->base.dirty) {
        text_layer->rs.buffers[0].data = (uint8_t *)vterm_screen_get_buffer(text_layer->screen);
        text_layer->rs.buffers[0].size = vterm_screen_get_buffer_size(text_layer->screen);
        text_layer->rs.buffers[0].dirty = 1;
    }

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
    /* Just set dirty flag - buffer is read directly from vterm in get_gpu_resource_set */
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

    /* Notify cursor callback */
    if (text_layer->base.cursor_fn) {
        text_layer->base.cursor_fn(&text_layer->base, pos.col, pos.row,
                                   text_layer->base.cursor_userdata);
    }
    return 1;
}

static int on_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    struct yetty_term_terminal_text_layer *text_layer = user;
    (void)cols;
    (void)cells;

    /* Notify scroll callback - 1 line scrolled down */
    if (text_layer->base.scroll_fn) {
        text_layer->base.scroll_fn(&text_layer->base, 1, text_layer->base.scroll_userdata);
    }
    return 1;
}
