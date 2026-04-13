// YPaint Canvas - Spatial grid for GPU primitive lookup
// Rolling offset approach: O(1) scroll, no per-primitive updates

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_yetty_ypaint_canvas;

// High bit marker for glyph indices in packed grid
#define YPAINT_GLYPH_BIT 0x80000000u

// Create a canvas
// @param scrolling_mode If true, primitives are cursor-relative and scroll
struct yetty_yetty_ypaint_canvas *yetty_yetty_ypaint_canvas_create(bool scrolling_mode);

// Destroy a canvas
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_destroy(struct yetty_yetty_ypaint_canvas *canvas);

//=============================================================================
// Configuration
//=============================================================================

// Set grid cell size (pixels)
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_set_cell_size(struct yetty_yetty_ypaint_canvas *canvas,
                            struct pixel_size pixel_size);
// Set grid dimensions (cols/rows)
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_set_grid_size(struct yetty_yetty_ypaint_canvas *canvas,
                            struct grid_size grid_size);

//=============================================================================
// Accessors
//=============================================================================

bool yetty_yetty_ypaint_canvas_scrolling_mode(struct yetty_yetty_ypaint_canvas *canvas);

struct pixel_size
yetty_yetty_ypaint_canvas_cell_get_pixel_size(struct yetty_yetty_ypaint_canvas *canvas);

struct grid_size yetty_yetty_ypaint_canvas_get_grid_size(struct yetty_yetty_ypaint_canvas *canvas);

uint32_t yetty_yetty_ypaint_canvas_line_count(struct yetty_yetty_ypaint_canvas *canvas);

//=============================================================================
// Cursor (scrolling mode only)
//=============================================================================

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_set_cursor_pos(struct yetty_yetty_ypaint_canvas *canvas,
                             struct grid_cursor_pos grid_cursor_pos);

//=============================================================================
// Rolling offset (for shader uniform)
//=============================================================================

// Get rolling_row_0: absolute row of visible line 0 (pass to shader as
// row_origin)
uint32_t yetty_yetty_ypaint_canvas_rolling_row_0(struct yetty_yetty_ypaint_canvas *canvas);

//=============================================================================
// Buffer management
//=============================================================================

struct yetty_ypaint_core_buffer;

// Add all primitives from a buffer to canvas
// Computes AABB for each primitive, tracks max_row, handles scrolling
// In scrolling mode: primitives positioned relative to cursor
// In non-scrolling mode: primitives positioned at absolute scene coordinates
struct yetty_core_void_result
yetty_ypaint_canvas_add_buffer(struct yetty_yetty_ypaint_canvas *canvas,
                         struct yetty_ypaint_core_buffer *buffer);

//=============================================================================
// Scrolling
//=============================================================================

// Scroll callback: called when primitive insertion requires scrolling
// @param user_data User data pointer
// @param num_lines Number of lines to scroll
typedef struct yetty_core_void_result (*yetty_yetty_ypaint_canvas_scroll_callback)(
    struct yetty_core_void_result *user_data, uint16_t num_lines);

// Cursor set callback: called when cursor moves WITHOUT scrolling
// @param user_data User data pointer
// @param new_row New cursor row position
typedef struct yetty_core_void_result (*yetty_yetty_ypaint_canvas_cursor_set_callback)(
    struct yetty_core_void_result *user_data, uint16_t new_row);

// Set scroll callback (called when add_buffer triggers scroll)
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_set_scroll_callback(struct yetty_yetty_ypaint_canvas *canvas,
                                  yetty_yetty_ypaint_canvas_scroll_callback callback,
                                  struct yetty_core_void_result *user_data);

// Set cursor callback (called when cursor moves without scroll)
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_set_cursor_callback(struct yetty_yetty_ypaint_canvas *canvas,
                                  yetty_yetty_ypaint_canvas_cursor_set_callback callback,
                                  struct yetty_core_void_result *user_data);

// Remove N lines from the top - primitives in those lines are deleted
// Only valid in scrolling mode
// O(1) for offset update, O(n) only for deleting the actual lines
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_scroll_lines(struct yetty_yetty_ypaint_canvas *canvas, uint16_t num_lines);

//=============================================================================
// Packed GPU format
//=============================================================================

// Mark grid as dirty (needs rebuild)
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_mark_dirty(struct yetty_yetty_ypaint_canvas *canvas);

// Check if grid needs rebuild
bool yetty_yetty_ypaint_canvas_is_dirty(struct yetty_yetty_ypaint_canvas *canvas);

// Rebuild packed grid format for GPU upload
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_rebuild_grid(struct yetty_yetty_ypaint_canvas *canvas);

// Glyph bounds callback: returns AABB for glyph at index
typedef struct yetty_core_void_result (*yetty_yetty_ypaint_canvas_glyph_bounds_func)(
    void *user_data, uint32_t index, float *min_x, float *min_y, float *max_x,
    float *max_y);

// Rebuild packed grid with glyphs
struct yetty_core_void_result yetty_yetty_ypaint_canvas_rebuild_grid_with_glyphs(
    struct yetty_yetty_ypaint_canvas *canvas, uint32_t glyph_count,
    yetty_yetty_ypaint_canvas_glyph_bounds_func bounds_func,
    struct yetty_core_void_result *user_data);

// Get packed grid data for GPU upload
const uint32_t *yetty_yetty_ypaint_canvas_grid_data(struct yetty_yetty_ypaint_canvas *canvas);
uint32_t yetty_yetty_ypaint_canvas_grid_word_count(struct yetty_yetty_ypaint_canvas *canvas);

// Clear packed grid staging
struct yetty_core_void_result
yetty_yetty_ypaint_canvas_clear_staging(struct yetty_yetty_ypaint_canvas *canvas);

//=============================================================================
// Primitive staging for GPU
//=============================================================================

// Build primitive staging data for GPU upload
// Returns pointer to staging buffer, sets word_count
const uint32_t *yetty_yetty_ypaint_canvas_build_prim_staging(struct yetty_yetty_ypaint_canvas *canvas,
                                                 uint32_t *word_count);

// Get total GPU size for primitives (in bytes)
uint32_t yetty_yetty_ypaint_canvas_prim_gpu_size(struct yetty_yetty_ypaint_canvas *canvas);

//=============================================================================
// State management
//=============================================================================

// Clear all lines and primitives
struct yetty_core_void_result yetty_yetty_ypaint_canvas_clear(struct yetty_yetty_ypaint_canvas *canvas);

// Check if canvas has any content
bool yetty_yetty_ypaint_canvas_empty(struct yetty_yetty_ypaint_canvas *canvas);

// Get total primitive count across all lines
uint32_t yetty_yetty_ypaint_canvas_primitive_count(struct yetty_yetty_ypaint_canvas *canvas);

#ifdef __cplusplus
}
#endif
