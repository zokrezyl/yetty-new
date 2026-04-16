// yplot - Plot complex primitive for ypaint

#include <yetty/yplot/yplot.h>

#include <stdlib.h>
#include <string.h>

//=============================================================================
// Default color palette
//=============================================================================

static const uint32_t COLOR_PALETTE[8] = {
    0xFFFF6B6B,  // Red
    0xFF4ECDC4,  // Teal
    0xFFFFE66D,  // Yellow
    0xFF95E1D3,  // Mint
    0xFFF38181,  // Coral
    0xFFAA96DA,  // Purple
    0xFF72D6C9,  // Cyan
    0xFFFCBF49,  // Orange
};

uint32_t yetty_yplot_default_color(uint32_t index)
{
    return COLOR_PALETTE[index % 8];
}

//=============================================================================
// Internal struct - base MUST be first
//=============================================================================

struct yetty_yplot_yplot {
    struct yetty_ypaint_core_complex_prim base;

    uint32_t *bytecode;
    uint32_t bytecode_word_count;
    uint32_t function_count;

    struct yetty_yplot_function functions[YETTY_YPLOT_MAX_FUNCTIONS];
    struct yetty_yplot_range range;
    struct yetty_yplot_axis axis;
    struct yetty_yplot_display display;

    float bounds_x, bounds_y, bounds_w, bounds_h;

    struct yetty_render_gpu_resource_set resource_set;
    struct yetty_render_buffer bytecode_buffer;
    struct yetty_render_buffer color_buffer;

    bool dirty;
};

//=============================================================================
// Forward declarations for ops
//=============================================================================

static void yplot_destroy(struct yetty_ypaint_core_complex_prim *self);
static struct yetty_render_gpu_resource_set_result
yplot_get_gpu_resource_set(struct yetty_ypaint_core_complex_prim *self);

static const struct yetty_ypaint_core_complex_prim_ops yplot_ops = {
    .destroy = yplot_destroy,
    .get_gpu_resource_set = yplot_get_gpu_resource_set,
};

//=============================================================================
// Config initialization
//=============================================================================

void yetty_yplot_range_init(struct yetty_yplot_range *range)
{
    range->x_min = YETTY_YPLOT_DEFAULT_X_MIN;
    range->x_max = YETTY_YPLOT_DEFAULT_X_MAX;
    range->y_min = YETTY_YPLOT_DEFAULT_Y_MIN;
    range->y_max = YETTY_YPLOT_DEFAULT_Y_MAX;
}

void yetty_yplot_axis_init(struct yetty_yplot_axis *axis)
{
    axis->show_grid = true;
    axis->show_axes = true;
    axis->show_labels = true;
    axis->axis_color = 0xFFAAAAAA;
    axis->grid_color = 0xFF444444;
    axis->label_color = 0xFFFFFFFF;
    axis->label_font_size = 10.0f;
    axis->grid_divisions_x = 10;
    axis->grid_divisions_y = 10;
}

void yetty_yplot_display_init(struct yetty_yplot_display *display)
{
    display->bg_color = 0xFF1A1A2E;
    display->zoom = 1.0f;
    display->center_x = 0.5f;
    display->center_y = 0.5f;
}

//=============================================================================
// Lifecycle
//=============================================================================

struct yetty_yplot_yplot_result yetty_yplot_yplot_create(void)
{
    struct yetty_yplot_yplot *plot = calloc(1, sizeof(struct yetty_yplot_yplot));
    if (!plot)
        return YETTY_ERR(yetty_yplot_yplot, "allocation failed");

    plot->base.ops = &yplot_ops;
    plot->base.type = YETTY_YPLOT_PRIM_TYPE_ID;
    plot->base.size = sizeof(struct yetty_yplot_yplot);

    yetty_yplot_range_init(&plot->range);
    yetty_yplot_axis_init(&plot->axis);
    yetty_yplot_display_init(&plot->display);

    for (int i = 0; i < YETTY_YPLOT_MAX_FUNCTIONS; i++) {
        plot->functions[i].color = yetty_yplot_default_color(i);
        plot->functions[i].visible = true;
    }

    plot->dirty = true;

    return YETTY_OK(yetty_yplot_yplot, plot);
}

void yetty_yplot_yplot_destroy(struct yetty_yplot_yplot *plot)
{
    if (!plot)
        return;
    free(plot->bytecode);
    free(plot->bytecode_buffer.data);
    free(plot->color_buffer.data);
    free(plot);
}

static void yplot_destroy(struct yetty_ypaint_core_complex_prim *self)
{
    yetty_yplot_yplot_destroy((struct yetty_yplot_yplot *)self);
}

//=============================================================================
// Bytecode
//=============================================================================

struct yetty_core_void_result
yetty_yplot_yplot_set_bytecode(struct yetty_yplot_yplot *plot,
                               const uint32_t *bytecode,
                               uint32_t word_count)
{
    if (!plot)
        return YETTY_ERR(yetty_core_void, "null plot");
    if (!bytecode || word_count < 4)
        return YETTY_ERR(yetty_core_void, "invalid bytecode");

    free(plot->bytecode);
    plot->bytecode = malloc(word_count * sizeof(uint32_t));
    if (!plot->bytecode)
        return YETTY_ERR(yetty_core_void, "allocation failed");

    memcpy(plot->bytecode, bytecode, word_count * sizeof(uint32_t));
    plot->bytecode_word_count = word_count;
    plot->function_count = bytecode[2];
    if (plot->function_count > YETTY_YPLOT_MAX_FUNCTIONS)
        plot->function_count = YETTY_YPLOT_MAX_FUNCTIONS;

    plot->dirty = true;
    return YETTY_OK_VOID();
}

const uint32_t *yetty_yplot_yplot_get_bytecode(struct yetty_yplot_yplot *plot,
                                                uint32_t *out_word_count)
{
    if (!plot) {
        if (out_word_count) *out_word_count = 0;
        return NULL;
    }
    if (out_word_count)
        *out_word_count = plot->bytecode_word_count;
    return plot->bytecode;
}

uint32_t yetty_yplot_yplot_function_count(struct yetty_yplot_yplot *plot)
{
    return plot ? plot->function_count : 0;
}

//=============================================================================
// Function styling
//=============================================================================

struct yetty_core_void_result
yetty_yplot_yplot_set_function_color(struct yetty_yplot_yplot *plot,
                                      uint32_t index, uint32_t color)
{
    if (!plot)
        return YETTY_ERR(yetty_core_void, "null plot");
    if (index >= YETTY_YPLOT_MAX_FUNCTIONS)
        return YETTY_ERR(yetty_core_void, "index out of range");
    plot->functions[index].color = color;
    plot->dirty = true;
    return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yplot_yplot_set_function_visible(struct yetty_yplot_yplot *plot,
                                        uint32_t index, bool visible)
{
    if (!plot)
        return YETTY_ERR(yetty_core_void, "null plot");
    if (index >= YETTY_YPLOT_MAX_FUNCTIONS)
        return YETTY_ERR(yetty_core_void, "index out of range");
    plot->functions[index].visible = visible;
    plot->dirty = true;
    return YETTY_OK_VOID();
}

uint32_t yetty_yplot_yplot_get_function_color(struct yetty_yplot_yplot *plot,
                                               uint32_t index)
{
    if (!plot || index >= YETTY_YPLOT_MAX_FUNCTIONS)
        return 0;
    return plot->functions[index].color;
}

bool yetty_yplot_yplot_get_function_visible(struct yetty_yplot_yplot *plot,
                                             uint32_t index)
{
    if (!plot || index >= YETTY_YPLOT_MAX_FUNCTIONS)
        return false;
    return plot->functions[index].visible;
}

//=============================================================================
// Configuration
//=============================================================================

struct yetty_yplot_range *yetty_yplot_yplot_range(struct yetty_yplot_yplot *plot)
{
    return plot ? &plot->range : NULL;
}

struct yetty_yplot_axis *yetty_yplot_yplot_axis(struct yetty_yplot_yplot *plot)
{
    return plot ? &plot->axis : NULL;
}

struct yetty_yplot_display *yetty_yplot_yplot_display(struct yetty_yplot_yplot *plot)
{
    return plot ? &plot->display : NULL;
}

void yetty_yplot_yplot_set_range(struct yetty_yplot_yplot *plot,
                                 float x_min, float x_max,
                                 float y_min, float y_max)
{
    if (!plot) return;
    plot->range.x_min = x_min;
    plot->range.x_max = x_max;
    plot->range.y_min = y_min;
    plot->range.y_max = y_max;
    plot->dirty = true;
}

//=============================================================================
// Spatial
//=============================================================================

void yetty_yplot_yplot_set_bounds(struct yetty_yplot_yplot *plot,
                                  float x, float y, float w, float h)
{
    if (!plot) return;
    plot->bounds_x = x;
    plot->bounds_y = y;
    plot->bounds_w = w;
    plot->bounds_h = h;
}

void yetty_yplot_yplot_get_aabb(struct yetty_yplot_yplot *plot,
                                float *min_x, float *min_y,
                                float *max_x, float *max_y)
{
    if (!plot) {
        if (min_x) *min_x = 0;
        if (min_y) *min_y = 0;
        if (max_x) *max_x = 0;
        if (max_y) *max_y = 0;
        return;
    }
    if (min_x) *min_x = plot->bounds_x;
    if (min_y) *min_y = plot->bounds_y;
    if (max_x) *max_x = plot->bounds_x + plot->bounds_w;
    if (max_y) *max_y = plot->bounds_y + plot->bounds_h;
}

//=============================================================================
// GPU Resource Set
//=============================================================================

bool yetty_yplot_yplot_is_dirty(struct yetty_yplot_yplot *plot)
{
    return plot ? plot->dirty : false;
}

void yetty_yplot_yplot_clear_dirty(struct yetty_yplot_yplot *plot)
{
    if (plot) plot->dirty = false;
}

struct yetty_render_gpu_resource_set_result
yetty_yplot_yplot_get_gpu_resource_set(struct yetty_yplot_yplot *plot)
{
    if (!plot)
        return YETTY_ERR(yetty_render_gpu_resource_set, "null plot");

    // Update bytecode buffer
    if (plot->bytecode && plot->bytecode_word_count > 0) {
        size_t size = plot->bytecode_word_count * sizeof(uint32_t);
        if (plot->bytecode_buffer.capacity < size) {
            free(plot->bytecode_buffer.data);
            plot->bytecode_buffer.data = malloc(size);
            plot->bytecode_buffer.capacity = size;
        }
        memcpy(plot->bytecode_buffer.data, plot->bytecode, size);
        plot->bytecode_buffer.size = size;
    }

    // Update color buffer
    size_t color_size = YETTY_YPLOT_MAX_FUNCTIONS * sizeof(uint32_t);
    if (plot->color_buffer.capacity < color_size) {
        free(plot->color_buffer.data);
        plot->color_buffer.data = malloc(color_size);
        plot->color_buffer.capacity = color_size;
    }
    uint32_t *colors = (uint32_t *)plot->color_buffer.data;
    for (int i = 0; i < YETTY_YPLOT_MAX_FUNCTIONS; i++)
        colors[i] = plot->functions[i].color;
    plot->color_buffer.size = color_size;

    // Build resource set
    memset(&plot->resource_set, 0, sizeof(plot->resource_set));
    strncpy(plot->resource_set.namespace, "yplot", YETTY_RENDER_NAME_MAX - 1);

    if (plot->bytecode_buffer.size > 0) {
        plot->resource_set.buffers[plot->resource_set.buffer_count] = plot->bytecode_buffer;
        strncpy(plot->resource_set.buffers[plot->resource_set.buffer_count].name,
                "bytecode", YETTY_RENDER_NAME_MAX - 1);
        plot->resource_set.buffer_count++;
    }

    plot->resource_set.buffers[plot->resource_set.buffer_count] = plot->color_buffer;
    strncpy(plot->resource_set.buffers[plot->resource_set.buffer_count].name,
            "colors", YETTY_RENDER_NAME_MAX - 1);
    plot->resource_set.buffer_count++;

    return YETTY_OK(yetty_render_gpu_resource_set, &plot->resource_set);
}

static struct yetty_render_gpu_resource_set_result
yplot_get_gpu_resource_set(struct yetty_ypaint_core_complex_prim *self)
{
    return yetty_yplot_yplot_get_gpu_resource_set((struct yetty_yplot_yplot *)self);
}

//=============================================================================
// Primitive serialization
//=============================================================================

uint32_t yetty_yplot_yplot_serialize_prim_header(struct yetty_yplot_yplot *plot,
                                                  uint32_t plot_index,
                                                  uint32_t col,
                                                  uint32_t rolling_row,
                                                  uint32_t z_order,
                                                  uint32_t *buffer,
                                                  uint32_t buffer_capacity)
{
    if (!plot)
        return 0;
    if (!buffer)
        return YETTY_YPLOT_PRIM_HEADER_WORDS;
    if (buffer_capacity < YETTY_YPLOT_PRIM_HEADER_WORDS)
        return 0;

    union { float f; uint32_t u; } conv;
    uint32_t i = 0;

    buffer[i++] = col | (rolling_row << 16);
    buffer[i++] = YETTY_YPLOT_PRIM_TYPE_ID;
    buffer[i++] = z_order;
    buffer[i++] = plot_index;

    uint32_t flags = 0;
    if (plot->axis.show_grid) flags |= YETTY_YPLOT_FLAG_GRID;
    if (plot->axis.show_axes) flags |= YETTY_YPLOT_FLAG_AXES;
    if (plot->axis.show_labels) flags |= YETTY_YPLOT_FLAG_LABELS;
    buffer[i++] = flags | (plot->function_count << 8);

    conv.f = plot->bounds_x; buffer[i++] = conv.u;
    conv.f = plot->bounds_y; buffer[i++] = conv.u;
    conv.f = plot->bounds_w; buffer[i++] = conv.u;
    conv.f = plot->bounds_h; buffer[i++] = conv.u;

    return YETTY_YPLOT_PRIM_HEADER_WORDS;
}

//=============================================================================
// Utilities
//=============================================================================

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct yetty_core_void_result
yetty_yplot_parse_color(const char *color_str, uint32_t *out_color)
{
    if (!color_str || !out_color)
        return YETTY_ERR(yetty_core_void, "null argument");

    const char *p = color_str;
    if (*p == '#') p++;

    size_t len = 0;
    const char *start = p;
    while (*p && hex_digit(*p) >= 0) { len++; p++; }

    if (len != 6 && len != 8)
        return YETTY_ERR(yetty_core_void, "color must be 6 or 8 hex digits");

    uint32_t value = 0;
    p = start;
    for (size_t i = 0; i < len; i++) {
        int d = hex_digit(*p++);
        value = (value << 4) | (uint32_t)d;
    }

    if (len == 6)
        value = 0xFF000000 | value;

    *out_color = value;
    return YETTY_OK_VOID();
}
