#include <yetty/term/text-layer.h>
#include <yetty/render/gpu-resource-set.h>
#include <vterm.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* container_of macro */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Text layer - embeds base as first member */
struct yetty_term_terminal_text_layer {
    struct yetty_term_terminal_layer base;
    VTerm *vterm;
    VTermScreen *screen;
};

/* Forward declarations */
static void text_layer_destroy(struct yetty_term_terminal_layer *self);
static void text_layer_write(struct yetty_term_terminal_layer *self,
                             const char *data, size_t len);
static void text_layer_resize(struct yetty_term_terminal_layer *self,
                              uint32_t cols, uint32_t rows);
static struct yetty_render_gpu_resource_set text_layer_get_gpu_resource_set(
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
    uint32_t cols, uint32_t rows)
{
    struct yetty_term_terminal_text_layer *text_layer;

    text_layer = calloc(1, sizeof(struct yetty_term_terminal_text_layer));
    if (!text_layer)
        return YETTY_ERR(yetty_term_terminal_layer, YETTY_ERR_NOMEM,
                         "failed to allocate text layer");

    text_layer->base.ops = &text_layer_ops;
    text_layer->base.cols = cols;
    text_layer->base.rows = rows;
    text_layer->base.cell_width = 10.0f;
    text_layer->base.cell_height = 20.0f;
    text_layer->base.dirty = 1;

    text_layer->vterm = vterm_new((int)rows, (int)cols);
    if (!text_layer->vterm) {
        free(text_layer);
        return YETTY_ERR(yetty_term_terminal_layer, YETTY_ERR_NOMEM,
                         "failed to create vterm");
    }

    vterm_set_utf8(text_layer->vterm, 1);

    text_layer->screen = vterm_obtain_screen(text_layer->vterm, NULL, NULL);
    vterm_screen_set_callbacks(text_layer->screen, &screen_callbacks, text_layer);
    vterm_screen_enable_altscreen(text_layer->screen, 1);
    vterm_screen_enable_reflow(text_layer->screen, 1);
    vterm_screen_reset(text_layer->screen, 1);

    return YETTY_OK(yetty_term_terminal_layer, &text_layer->base);
}

/* Ops implementations */

static void text_layer_destroy(struct yetty_term_terminal_layer *self)
{
    struct yetty_term_terminal_text_layer *text_layer;

    text_layer = container_of(self, struct yetty_term_terminal_text_layer, base);

    if (text_layer->vterm)
        vterm_free(text_layer->vterm);

    free(text_layer);
}

static void text_layer_write(struct yetty_term_terminal_layer *self,
                             const char *data, size_t len)
{
    struct yetty_term_terminal_text_layer *text_layer;

    text_layer = container_of(self, struct yetty_term_terminal_text_layer, base);

    if (text_layer->vterm && len > 0)
        vterm_input_write(text_layer->vterm, data, len);
}

static void text_layer_resize(struct yetty_term_terminal_layer *self,
                              uint32_t cols, uint32_t rows)
{
    struct yetty_term_terminal_text_layer *text_layer;

    text_layer = container_of(self, struct yetty_term_terminal_text_layer, base);

    if (text_layer->vterm) {
        vterm_set_size(text_layer->vterm, (int)rows, (int)cols);
        self->cols = cols;
        self->rows = rows;
    }
}

static struct yetty_render_gpu_resource_set text_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self)
{
    const struct yetty_term_terminal_text_layer *text_layer;
    struct yetty_render_gpu_resource_set resource_set = {0};
    const VTermScreenCell *cells;

    text_layer = container_of(self, struct yetty_term_terminal_text_layer, base);

    strncpy(resource_set.name, "textGrid",
            YETTY_RENDER_GPU_RESOURCE_NAME_MAX - 1);

    cells = vterm_screen_get_buffer(text_layer->screen);
    resource_set.buffer_data = (const uint8_t *)cells;
    resource_set.buffer_data_size = self->cols * self->rows * sizeof(VTermScreenCell);
    resource_set.buffer_size = resource_set.buffer_data_size;
    strncpy(resource_set.buffer_wgsl_type, "array<TextCell>",
            YETTY_RENDER_GPU_RESOURCE_WGSL_TYPE_MAX - 1);
    strncpy(resource_set.buffer_name, "textCells",
            YETTY_RENDER_GPU_RESOURCE_NAME_MAX - 1);
    resource_set.buffer_readonly = 1;

    return resource_set;
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

    (void)pos;
    (void)oldpos;
    (void)visible;
    text_layer->base.dirty = 1;

    return 1;
}
