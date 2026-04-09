// YPaint Canvas - Spatial grid for GPU primitive lookup
// Rolling offset approach: O(1) scroll, no per-primitive updates

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a YPaintCanvas
typedef struct YPaintCanvas* YPaintCanvasHandle;

// High bit marker for glyph indices in packed grid
#define YPAINT_GLYPH_BIT 0x80000000u

// Create a canvas
// @param scrollingMode If true, primitives are cursor-relative and scroll
YPaintCanvasHandle ypaint_canvas_create(bool scrollingMode);

// Destroy a canvas
void ypaint_canvas_destroy(YPaintCanvasHandle canvas);

//=============================================================================
// Configuration
//=============================================================================

// Set scene bounds (world coordinates)
void ypaint_canvas_set_scene_bounds(YPaintCanvasHandle canvas,
                                     float minX, float minY,
                                     float maxX, float maxY);

// Set grid cell size (scene units)
void ypaint_canvas_set_cell_size(YPaintCanvasHandle canvas,
                                  float sizeX, float sizeY);

// Set maximum primitives per cell (for GPU culling efficiency)
void ypaint_canvas_set_max_prims_per_cell(YPaintCanvasHandle canvas, uint32_t max);

//=============================================================================
// Accessors
//=============================================================================

bool ypaint_canvas_scrolling_mode(YPaintCanvasHandle canvas);

float ypaint_canvas_scene_min_x(YPaintCanvasHandle canvas);
float ypaint_canvas_scene_min_y(YPaintCanvasHandle canvas);
float ypaint_canvas_scene_max_x(YPaintCanvasHandle canvas);
float ypaint_canvas_scene_max_y(YPaintCanvasHandle canvas);

float ypaint_canvas_cell_size_x(YPaintCanvasHandle canvas);
float ypaint_canvas_cell_size_y(YPaintCanvasHandle canvas);
uint32_t ypaint_canvas_grid_width(YPaintCanvasHandle canvas);
uint32_t ypaint_canvas_grid_height(YPaintCanvasHandle canvas);
uint32_t ypaint_canvas_max_prims_per_cell(YPaintCanvasHandle canvas);

uint32_t ypaint_canvas_line_count(YPaintCanvasHandle canvas);
uint32_t ypaint_canvas_height_in_lines(YPaintCanvasHandle canvas);

//=============================================================================
// Cursor (scrolling mode only)
//=============================================================================

void ypaint_canvas_set_cursor(YPaintCanvasHandle canvas, uint16_t col, uint16_t row);
uint16_t ypaint_canvas_cursor_col(YPaintCanvasHandle canvas);
uint16_t ypaint_canvas_cursor_row(YPaintCanvasHandle canvas);

//=============================================================================
// Rolling offset (for shader uniform)
//=============================================================================

// Get the absolute row index of line 0 (pass to shader as uniform)
uint32_t ypaint_canvas_row0_absolute(YPaintCanvasHandle canvas);

//=============================================================================
// Primitive management
//=============================================================================

// Add a primitive with pre-computed AABB
// primData: raw primitive data WITHOUT gridOffset (type, zOrder, style, geometry)
// In scrolling mode: positioned relative to cursor
// In non-scrolling mode: positioned at absolute scene coordinates
void ypaint_canvas_add_primitive(YPaintCanvasHandle canvas,
                                  const float* primData, uint32_t wordCount,
                                  float aabbMinX, float aabbMinY,
                                  float aabbMaxX, float aabbMaxY);

//=============================================================================
// Scrolling
//=============================================================================

// Remove N lines from the top - primitives in those lines are deleted
// Only valid in scrolling mode
// O(1) for offset update, O(n) only for deleting the actual lines
void ypaint_canvas_scroll_lines(YPaintCanvasHandle canvas, uint16_t numLines);

//=============================================================================
// Packed GPU format
//=============================================================================

// Mark grid as dirty (needs rebuild)
void ypaint_canvas_mark_dirty(YPaintCanvasHandle canvas);

// Check if grid needs rebuild
bool ypaint_canvas_is_dirty(YPaintCanvasHandle canvas);

// Rebuild packed grid format for GPU upload
void ypaint_canvas_rebuild_grid(YPaintCanvasHandle canvas);

// Rebuild packed grid with glyphs
// glyphBoundsFunc: callback(userData, glyphIndex, &minX, &minY, &maxX, &maxY)
typedef void (*YPaintGlyphBoundsFunc)(void* userData, uint32_t index,
                                       float* minX, float* minY,
                                       float* maxX, float* maxY);
void ypaint_canvas_rebuild_grid_with_glyphs(YPaintCanvasHandle canvas,
                                             uint32_t glyphCount,
                                             YPaintGlyphBoundsFunc boundsFunc,
                                             void* userData);

// Get packed grid data for GPU upload
const uint32_t* ypaint_canvas_grid_data(YPaintCanvasHandle canvas);
uint32_t ypaint_canvas_grid_word_count(YPaintCanvasHandle canvas);

// Clear packed grid staging
void ypaint_canvas_clear_staging(YPaintCanvasHandle canvas);

//=============================================================================
// Primitive staging for GPU
//=============================================================================

// Build primitive staging data for GPU upload
// Returns pointer to staging buffer, sets wordCount
const uint32_t* ypaint_canvas_build_prim_staging(YPaintCanvasHandle canvas,
                                                  uint32_t* wordCount);

// Get total GPU size for primitives (in bytes)
uint32_t ypaint_canvas_prim_gpu_size(YPaintCanvasHandle canvas);

//=============================================================================
// State management
//=============================================================================

// Clear all lines and primitives
void ypaint_canvas_clear(YPaintCanvasHandle canvas);

// Check if canvas has any content
bool ypaint_canvas_empty(YPaintCanvasHandle canvas);

// Get total primitive count across all lines
uint32_t ypaint_canvas_primitive_count(YPaintCanvasHandle canvas);

#ifdef __cplusplus
}
#endif
