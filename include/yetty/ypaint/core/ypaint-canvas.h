// YPaint Canvas - Spatial grid for GPU primitive lookup
// Rolling offset approach: O(1) scroll, no per-primitive updates

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ypaint_canvas;

// High bit marker for glyph indices in packed grid
#define YPAINT_GLYPH_BIT 0x80000000u

// Create a canvas
// @param scrolling_mode If true, primitives are cursor-relative and scroll
struct ypaint_canvas *ypaint_canvas_create(bool scrolling_mode);

// Destroy a canvas
void ypaint_canvas_destroy(struct ypaint_canvas *canvas);

//=============================================================================
// Configuration
//=============================================================================

// Set scene bounds (world coordinates)
void ypaint_canvas_set_scene_bounds(struct ypaint_canvas *canvas,
				    float min_x, float min_y,
				    float max_x, float max_y);

// Set grid cell size (scene units)
void ypaint_canvas_set_cell_size(struct ypaint_canvas *canvas,
				 float size_x, float size_y);

// Set maximum primitives per cell (for GPU culling efficiency)
void ypaint_canvas_set_max_prims_per_cell(struct ypaint_canvas *canvas,
					  uint32_t max);

//=============================================================================
// Accessors
//=============================================================================

bool ypaint_canvas_scrolling_mode(struct ypaint_canvas *canvas);

float ypaint_canvas_scene_min_x(struct ypaint_canvas *canvas);
float ypaint_canvas_scene_min_y(struct ypaint_canvas *canvas);
float ypaint_canvas_scene_max_x(struct ypaint_canvas *canvas);
float ypaint_canvas_scene_max_y(struct ypaint_canvas *canvas);

float ypaint_canvas_cell_size_x(struct ypaint_canvas *canvas);
float ypaint_canvas_cell_size_y(struct ypaint_canvas *canvas);
uint32_t ypaint_canvas_grid_width(struct ypaint_canvas *canvas);
uint32_t ypaint_canvas_grid_height(struct ypaint_canvas *canvas);
uint32_t ypaint_canvas_max_prims_per_cell(struct ypaint_canvas *canvas);

uint32_t ypaint_canvas_line_count(struct ypaint_canvas *canvas);
uint32_t ypaint_canvas_height_in_lines(struct ypaint_canvas *canvas);

//=============================================================================
// Cursor (scrolling mode only)
//=============================================================================

void ypaint_canvas_set_cursor(struct ypaint_canvas *canvas,
			      uint16_t col, uint16_t row);
uint16_t ypaint_canvas_cursor_col(struct ypaint_canvas *canvas);
uint16_t ypaint_canvas_cursor_row(struct ypaint_canvas *canvas);

//=============================================================================
// Rolling offset (for shader uniform)
//=============================================================================

// Get the absolute row index of line 0 (pass to shader as uniform)
uint32_t ypaint_canvas_row0_absolute(struct ypaint_canvas *canvas);

//=============================================================================
// Primitive management
//=============================================================================

// Add a primitive with pre-computed AABB
// prim_data: raw primitive data WITHOUT grid_offset (type, z_order, style, geometry)
// In scrolling mode: positioned relative to cursor
// In non-scrolling mode: positioned at absolute scene coordinates
void ypaint_canvas_add_primitive(struct ypaint_canvas *canvas,
				 const float *prim_data, uint32_t word_count,
				 float aabb_min_x, float aabb_min_y,
				 float aabb_max_x, float aabb_max_y);

// Commit buffer after adding primitives
// Handles auto-scroll if primitives extend beyond visible area
// @param max_row The maximum row reached by primitives in this buffer
void ypaint_canvas_commit_buffer(struct ypaint_canvas *canvas, uint32_t max_row);

//=============================================================================
// Scrolling
//=============================================================================

// Scroll callback: called when primitive insertion requires scrolling
// @param user_data User data pointer
// @param num_lines Number of lines to scroll
typedef void (*ypaint_canvas_scroll_callback)(void *user_data, uint16_t num_lines);

// Set scroll callback (called when add_primitive triggers scroll)
void ypaint_canvas_set_scroll_callback(struct ypaint_canvas *canvas,
				       ypaint_canvas_scroll_callback callback,
				       void *user_data);

// Remove N lines from the top - primitives in those lines are deleted
// Only valid in scrolling mode
// O(1) for offset update, O(n) only for deleting the actual lines
void ypaint_canvas_scroll_lines(struct ypaint_canvas *canvas, uint16_t num_lines);

//=============================================================================
// Packed GPU format
//=============================================================================

// Mark grid as dirty (needs rebuild)
void ypaint_canvas_mark_dirty(struct ypaint_canvas *canvas);

// Check if grid needs rebuild
bool ypaint_canvas_is_dirty(struct ypaint_canvas *canvas);

// Rebuild packed grid format for GPU upload
void ypaint_canvas_rebuild_grid(struct ypaint_canvas *canvas);

// Glyph bounds callback: returns AABB for glyph at index
typedef void (*ypaint_canvas_glyph_bounds_func)(void *user_data, uint32_t index,
						float *min_x, float *min_y,
						float *max_x, float *max_y);

// Rebuild packed grid with glyphs
void ypaint_canvas_rebuild_grid_with_glyphs(struct ypaint_canvas *canvas,
					    uint32_t glyph_count,
					    ypaint_canvas_glyph_bounds_func bounds_func,
					    void *user_data);

// Get packed grid data for GPU upload
const uint32_t *ypaint_canvas_grid_data(struct ypaint_canvas *canvas);
uint32_t ypaint_canvas_grid_word_count(struct ypaint_canvas *canvas);

// Clear packed grid staging
void ypaint_canvas_clear_staging(struct ypaint_canvas *canvas);

//=============================================================================
// Primitive staging for GPU
//=============================================================================

// Build primitive staging data for GPU upload
// Returns pointer to staging buffer, sets word_count
const uint32_t *ypaint_canvas_build_prim_staging(struct ypaint_canvas *canvas,
						 uint32_t *word_count);

// Get total GPU size for primitives (in bytes)
uint32_t ypaint_canvas_prim_gpu_size(struct ypaint_canvas *canvas);

//=============================================================================
// State management
//=============================================================================

// Clear all lines and primitives
void ypaint_canvas_clear(struct ypaint_canvas *canvas);

// Check if canvas has any content
bool ypaint_canvas_empty(struct ypaint_canvas *canvas);

// Get total primitive count across all lines
uint32_t ypaint_canvas_primitive_count(struct ypaint_canvas *canvas);

#ifdef __cplusplus
}
#endif
