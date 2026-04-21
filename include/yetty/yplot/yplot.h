// yplot - Plot complex primitive for ypaint
// Pure C API - accepts pre-compiled yfsvm bytecode
//
// yplot is a "complex primitive" in ypaint:
// - Has spatial presence in the ypaint canvas (AABB for culling)
// - Has its own gpu_resource_set (bytecode buffer, color table)
// - Referenced from ypaint canvas like other primitives

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <yetty/ycore/result.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/ypaint-core/complex-prim.h>
#include <yetty/ypaint-core/complex-prim-types.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Constants
//=============================================================================

#define YETTY_YPLOT_MAX_FUNCTIONS 8
#define YETTY_YPLOT_DEFAULT_X_MIN -3.14159f
#define YETTY_YPLOT_DEFAULT_X_MAX  3.14159f
#define YETTY_YPLOT_DEFAULT_Y_MIN -1.5f
#define YETTY_YPLOT_DEFAULT_Y_MAX  1.5f

// Primitive type ID - use complex prim range
#define YETTY_YPLOT_PRIM_TYPE_ID YETTY_YPAINT_TYPE_YPLOT

// Flags
#define YETTY_YPLOT_FLAG_GRID   0x01
#define YETTY_YPLOT_FLAG_AXES   0x02
#define YETTY_YPLOT_FLAG_LABELS 0x04
#define YETTY_YPLOT_FLAG_BUFFER 0x08  // Buffer mode vs expression mode

//=============================================================================
// Configuration structs
//=============================================================================

struct yetty_yplot_function {
    uint32_t color;       // ARGB
    bool visible;
};

struct yetty_yplot_range {
    float x_min;
    float x_max;
    float y_min;
    float y_max;
};

struct yetty_yplot_axis {
    bool show_grid;
    bool show_axes;
    bool show_labels;
    uint32_t axis_color;
    uint32_t grid_color;
    uint32_t label_color;
    float label_font_size;
    int grid_divisions_x;
    int grid_divisions_y;
};

struct yetty_yplot_display {
    uint32_t bg_color;
    float zoom;
    float center_x;
    float center_y;
};

//=============================================================================
// yetty_yplot_yplot - main plot object
//=============================================================================

struct yetty_yplot_yplot;

//=============================================================================
// Result types
//=============================================================================

YETTY_RESULT_DECLARE(yetty_yplot_yplot, struct yetty_yplot_yplot *);

//=============================================================================
// Lifecycle
//=============================================================================

struct yetty_yplot_yplot_result yetty_yplot_yplot_create(void);
void yetty_yplot_yplot_destroy(struct yetty_yplot_yplot *plot);

//=============================================================================
// Bytecode management
//=============================================================================

struct yetty_core_void_result
yetty_yplot_yplot_set_bytecode(struct yetty_yplot_yplot *plot,
                               const uint32_t *bytecode,
                               uint32_t word_count);

const uint32_t *yetty_yplot_yplot_get_bytecode(struct yetty_yplot_yplot *plot,
                                                uint32_t *out_word_count);

uint32_t yetty_yplot_yplot_function_count(struct yetty_yplot_yplot *plot);

//=============================================================================
// Function styling
//=============================================================================

struct yetty_core_void_result
yetty_yplot_yplot_set_function_color(struct yetty_yplot_yplot *plot,
                                      uint32_t index, uint32_t color);

struct yetty_core_void_result
yetty_yplot_yplot_set_function_visible(struct yetty_yplot_yplot *plot,
                                        uint32_t index, bool visible);

uint32_t yetty_yplot_yplot_get_function_color(struct yetty_yplot_yplot *plot,
                                               uint32_t index);

bool yetty_yplot_yplot_get_function_visible(struct yetty_yplot_yplot *plot,
                                             uint32_t index);

//=============================================================================
// Configuration
//=============================================================================

struct yetty_yplot_range *yetty_yplot_yplot_range(struct yetty_yplot_yplot *plot);
struct yetty_yplot_axis *yetty_yplot_yplot_axis(struct yetty_yplot_yplot *plot);
struct yetty_yplot_display *yetty_yplot_yplot_display(struct yetty_yplot_yplot *plot);

void yetty_yplot_yplot_set_range(struct yetty_yplot_yplot *plot,
                                 float x_min, float x_max,
                                 float y_min, float y_max);

void yetty_yplot_range_init(struct yetty_yplot_range *range);
void yetty_yplot_axis_init(struct yetty_yplot_axis *axis);
void yetty_yplot_display_init(struct yetty_yplot_display *display);

//=============================================================================
// Spatial (for ypaint canvas integration)
//=============================================================================

void yetty_yplot_yplot_set_bounds(struct yetty_yplot_yplot *plot,
                                  float x, float y,
                                  float width, float height);

void yetty_yplot_yplot_get_aabb(struct yetty_yplot_yplot *plot,
                                float *min_x, float *min_y,
                                float *max_x, float *max_y);

//=============================================================================
// GPU Resource Set
//=============================================================================

struct yetty_render_gpu_resource_set_result
yetty_yplot_yplot_get_gpu_resource_set(struct yetty_yplot_yplot *plot);

// Get static shader-only resource set (for ypaint layer to include yplot_render)
// This resource set contains only shader code, no buffers.
const struct yetty_render_gpu_resource_set *yetty_yplot_get_shader_resource_set(void);

bool yetty_yplot_yplot_is_dirty(struct yetty_yplot_yplot *plot);
void yetty_yplot_yplot_clear_dirty(struct yetty_yplot_yplot *plot);

//=============================================================================
// Primitive serialization
//=============================================================================

#define YETTY_YPLOT_PRIM_HEADER_WORDS 9

uint32_t yetty_yplot_yplot_serialize_prim_header(struct yetty_yplot_yplot *plot,
                                                  uint32_t plot_index,
                                                  uint32_t col,
                                                  uint32_t rolling_row,
                                                  uint32_t z_order,
                                                  uint32_t *buffer,
                                                  uint32_t buffer_capacity);

//=============================================================================
// Wire format (for complex prim buffer storage)
//=============================================================================

// Register yplot with global complex prim type registry (call at init)
void yetty_yplot_register(void);

// Register yplot with a specific factory instance
struct yetty_core_void_result yetty_yplot_register_to_factory(
    struct yetty_ypaint_complex_prim_factory *factory);

// Flyweight handler for buffer iteration (legacy, for backward compatibility)
#include <yetty/ypaint-core/flyweight.h>
struct yetty_ypaint_prim_ops_ptr_result yetty_yplot_handler(uint32_t prim_type);

// Serialize yplot to wire format payload (returns payload size, or 0 on error)
// If buffer is NULL, returns required size
uint32_t yetty_yplot_yplot_serialize_wire(struct yetty_yplot_yplot *plot,
                                           uint8_t *buffer,
                                           uint32_t buffer_capacity);

//=============================================================================
// Utilities
//=============================================================================

struct yetty_core_void_result
yetty_yplot_parse_color(const char *color_str, uint32_t *out_color);

uint32_t yetty_yplot_default_color(uint32_t index);

#ifdef __cplusplus
}
#endif
