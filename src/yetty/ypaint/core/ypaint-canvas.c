// YPaint Canvas - Implementation
// Rolling offset approach for O(1) scrolling

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <yetty/ypaint/core/ypaint-canvas.h>

//=============================================================================
// Internal data structures
//=============================================================================

// Reference to a primitive in another line
struct ypaint_canvas_prim_ref {
  uint16_t lines_ahead; // relative offset to base line (0 = same line)
  uint16_t prim_index;  // index within base line's prims array
};

// Dynamic array of prim_ref
struct ypaint_canvas_prim_ref_array {
  struct ypaint_canvas_prim_ref *data;
  uint32_t count;
  uint32_t capacity;
};

// A single grid cell
struct ypaint_canvas_grid_cell {
  struct ypaint_canvas_prim_ref_array refs;
};

// A single primitive's data
struct ypaint_canvas_prim_data {
  uint32_t rolling_row; // rolling_row at insertion (cursor row or explicit)
  float *data;
  uint32_t word_count;
};

// Dynamic array of prim_data
struct ypaint_canvas_prim_data_array {
  struct ypaint_canvas_prim_data *data;
  uint32_t count;
  uint32_t capacity;
};

// A single row/line in the grid
struct ypaint_canvas_grid_line {
  struct ypaint_canvas_prim_data_array
      prims;                             // primitives whose BASE is this line
  struct ypaint_canvas_grid_cell *cells; // grid cells for this line
  uint32_t cell_count;
  uint32_t cell_capacity;
  uint32_t rolling_row; // absolute row number, never changes after creation
};

// Circular buffer for lines (deque-like)
struct ypaint_canvas_line_buffer {
  struct ypaint_canvas_grid_line *lines;
  uint32_t capacity;
  uint32_t head;  // index of first valid line
  uint32_t count; // number of valid lines
};

// Canvas structure
struct ypaint_canvas {
  bool scrolling_mode;

  // Scene bounds
  float scene_min_x;
  float scene_min_y;
  float scene_max_x;
  float scene_max_y;

  // Cell size
  float cell_size_x;
  float cell_size_y;

  // Grid dimensions
  uint32_t grid_width;
  uint32_t grid_height;
  uint32_t max_prims_per_cell;

  // Cursor (scrolling mode)
  uint16_t cursor_col;
  uint16_t cursor_row;

  // Rolling offset: absolute row index of line 0
  uint32_t row0_absolute;

  // Next rolling row to assign to new lines
  uint32_t next_rolling_row;

  // Lines
  struct ypaint_canvas_line_buffer lines;

  // Packed grid staging
  uint32_t *grid_staging;
  uint32_t grid_staging_count;
  uint32_t grid_staging_capacity;
  bool dirty;

  // Primitive staging
  uint32_t *prim_staging;
  uint32_t prim_staging_count;
  uint32_t prim_staging_capacity;

  // Scroll callback
  ypaint_canvas_scroll_callback scroll_callback;
  void *scroll_callback_user_data;
};

#define DEFAULT_MAX_PRIMS_PER_CELL 16
#define INITIAL_LINE_CAPACITY 64
#define INITIAL_CELL_CAPACITY 128
#define INITIAL_PRIM_CAPACITY 16
#define INITIAL_REF_CAPACITY 8
#define INITIAL_STAGING_CAPACITY 4096

//=============================================================================
// Helper: Dynamic arrays
//=============================================================================

static void prim_ref_array_init(struct ypaint_canvas_prim_ref_array *arr) {
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static void prim_ref_array_free(struct ypaint_canvas_prim_ref_array *arr) {
  free(arr->data);
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static void prim_ref_array_push(struct ypaint_canvas_prim_ref_array *arr,
                                struct ypaint_canvas_prim_ref ref) {
  if (arr->count >= arr->capacity) {
    uint32_t new_cap =
        arr->capacity == 0 ? INITIAL_REF_CAPACITY : arr->capacity * 2;
    arr->data =
        realloc(arr->data, new_cap * sizeof(struct ypaint_canvas_prim_ref));
    arr->capacity = new_cap;
  }
  arr->data[arr->count++] = ref;
}

static void prim_data_array_init(struct ypaint_canvas_prim_data_array *arr) {
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static void prim_data_array_free(struct ypaint_canvas_prim_data_array *arr) {
  for (uint32_t i = 0; i < arr->count; i++)
    free(arr->data[i].data);
  free(arr->data);
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static uint32_t prim_data_array_push(struct ypaint_canvas_prim_data_array *arr,
                                     uint32_t rolling_row, const float *data,
                                     uint32_t word_count) {
  if (arr->count >= arr->capacity) {
    uint32_t new_cap =
        arr->capacity == 0 ? INITIAL_PRIM_CAPACITY : arr->capacity * 2;
    arr->data =
        realloc(arr->data, new_cap * sizeof(struct ypaint_canvas_prim_data));
    arr->capacity = new_cap;
  }
  uint32_t idx = arr->count++;
  arr->data[idx].rolling_row = rolling_row;
  arr->data[idx].data = malloc(word_count * sizeof(float));
  arr->data[idx].word_count = word_count;
  memcpy(arr->data[idx].data, data, word_count * sizeof(float));
  return idx;
}

//=============================================================================
// Helper: grid_line
//=============================================================================

static void grid_line_init(struct ypaint_canvas_grid_line *line,
                           uint32_t initial_cells, uint32_t rolling_row) {
  prim_data_array_init(&line->prims);
  line->cells = NULL;
  line->cell_count = 0;
  line->cell_capacity = 0;
  line->rolling_row = rolling_row;
  if (initial_cells > 0) {
    line->cells = calloc(initial_cells, sizeof(struct ypaint_canvas_grid_cell));
    line->cell_capacity = initial_cells;
  }
}

static void grid_line_free(struct ypaint_canvas_grid_line *line) {
  prim_data_array_free(&line->prims);
  for (uint32_t i = 0; i < line->cell_count; i++)
    prim_ref_array_free(&line->cells[i].refs);
  free(line->cells);
  line->cells = NULL;
  line->cell_count = 0;
  line->cell_capacity = 0;
}

static void grid_line_ensure_cells(struct ypaint_canvas_grid_line *line,
                                   uint32_t min_cells) {
  if (min_cells <= line->cell_capacity) {
    if (min_cells > line->cell_count) {
      for (uint32_t i = line->cell_count; i < min_cells; i++)
        prim_ref_array_init(&line->cells[i].refs);
      line->cell_count = min_cells;
    }
    return;
  }

  uint32_t new_cap =
      line->cell_capacity == 0 ? INITIAL_CELL_CAPACITY : line->cell_capacity;
  while (new_cap < min_cells)
    new_cap *= 2;

  line->cells =
      realloc(line->cells, new_cap * sizeof(struct ypaint_canvas_grid_cell));
  for (uint32_t i = line->cell_capacity; i < new_cap; i++)
    prim_ref_array_init(&line->cells[i].refs);
  line->cell_capacity = new_cap;
  line->cell_count = min_cells;
}

//=============================================================================
// Helper: line_buffer (circular buffer)
//=============================================================================

static void line_buffer_init(struct ypaint_canvas_line_buffer *buf) {
  buf->lines = NULL;
  buf->capacity = 0;
  buf->head = 0;
  buf->count = 0;
}

static void line_buffer_free(struct ypaint_canvas_line_buffer *buf) {
  for (uint32_t i = 0; i < buf->count; i++) {
    uint32_t idx = (buf->head + i) % buf->capacity;
    grid_line_free(&buf->lines[idx]);
  }
  free(buf->lines);
  buf->lines = NULL;
  buf->capacity = 0;
  buf->head = 0;
  buf->count = 0;
}

static struct ypaint_canvas_grid_line *
line_buffer_get(struct ypaint_canvas_line_buffer *buf, uint32_t index) {
  if (index >= buf->count)
    return NULL;
  uint32_t idx = (buf->head + index) % buf->capacity;
  return &buf->lines[idx];
}

static void canvas_ensure_lines(struct ypaint_canvas *canvas,
                                uint32_t min_count) {
  struct ypaint_canvas_line_buffer *buf = &canvas->lines;

  // Grow capacity if needed
  if (min_count > buf->capacity) {
    uint32_t new_cap =
        buf->capacity == 0 ? INITIAL_LINE_CAPACITY : buf->capacity;
    while (new_cap < min_count)
      new_cap *= 2;

    struct ypaint_canvas_grid_line *new_lines;
    new_lines = calloc(new_cap, sizeof(struct ypaint_canvas_grid_line));

    // Copy existing lines to new buffer (linearize)
    for (uint32_t i = 0; i < buf->count; i++) {
      uint32_t old_idx = (buf->head + i) % buf->capacity;
      new_lines[i] = buf->lines[old_idx];
    }

    free(buf->lines);
    buf->lines = new_lines;
    buf->capacity = new_cap;
    buf->head = 0;
  }

  // Initialize new lines with incrementing rolling_row
  while (buf->count < min_count) {
    uint32_t idx = (buf->head + buf->count) % buf->capacity;
    grid_line_init(&buf->lines[idx], canvas->grid_width,
                   canvas->next_rolling_row);
    canvas->next_rolling_row++;
    buf->count++;
  }
}

static void line_buffer_pop_front(struct ypaint_canvas_line_buffer *buf,
                                  uint32_t count) {
  for (uint32_t i = 0; i < count && buf->count > 0; i++) {
    grid_line_free(&buf->lines[buf->head]);
    grid_line_init(&buf->lines[buf->head], 0,
                   0); // rolling_row irrelevant, line removed
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count--;
  }
}

//=============================================================================
// Canvas implementation
//=============================================================================

struct ypaint_canvas *ypaint_canvas_create(bool scrolling_mode) {
  struct ypaint_canvas *canvas;

  canvas = calloc(1, sizeof(struct ypaint_canvas));
  if (!canvas)
    return NULL;

  canvas->scrolling_mode = scrolling_mode;
  canvas->max_prims_per_cell = DEFAULT_MAX_PRIMS_PER_CELL;
  canvas->dirty = true;
  canvas->row0_absolute = 0;

  line_buffer_init(&canvas->lines);

  return canvas;
}

void ypaint_canvas_destroy(struct ypaint_canvas *canvas) {
  if (!canvas)
    return;

  line_buffer_free(&canvas->lines);
  free(canvas->grid_staging);
  free(canvas->prim_staging);
  free(canvas);
}

//=============================================================================
// Configuration
//=============================================================================

static void update_grid_dimensions(struct ypaint_canvas *canvas) {
  if (canvas->cell_size_x <= 0.0f || canvas->cell_size_y <= 0.0f) {
    canvas->grid_width = 0;
    canvas->grid_height = 0;
    return;
  }

  float scene_w = canvas->scene_max_x - canvas->scene_min_x;
  float scene_h = canvas->scene_max_y - canvas->scene_min_y;

  if (scene_w <= 0.0f || scene_h <= 0.0f) {
    canvas->grid_width = 0;
    canvas->grid_height = 0;
    return;
  }

  canvas->grid_width = (uint32_t)ceilf(scene_w / canvas->cell_size_x);
  canvas->grid_height = (uint32_t)ceilf(scene_h / canvas->cell_size_y);
  if (canvas->grid_width < 1)
    canvas->grid_width = 1;
  if (canvas->grid_height < 1)
    canvas->grid_height = 1;
}

void ypaint_canvas_set_scene_bounds(struct ypaint_canvas *canvas, float min_x,
                                    float min_y, float max_x, float max_y) {
  if (!canvas)
    return;
  canvas->scene_min_x = min_x;
  canvas->scene_min_y = min_y;
  canvas->scene_max_x = max_x;
  canvas->scene_max_y = max_y;
  update_grid_dimensions(canvas);
  canvas->dirty = true;
}

void ypaint_canvas_set_cell_size(struct ypaint_canvas *canvas, float size_x,
                                 float size_y) {
  if (!canvas)
    return;
  canvas->cell_size_x = size_x;
  canvas->cell_size_y = size_y;
  update_grid_dimensions(canvas);
  canvas->dirty = true;
}

void ypaint_canvas_set_max_prims_per_cell(struct ypaint_canvas *canvas,
                                          uint32_t max) {
  if (canvas)
    canvas->max_prims_per_cell = max;
}

//=============================================================================
// Accessors
//=============================================================================

bool ypaint_canvas_scrolling_mode(struct ypaint_canvas *canvas) {
  return canvas ? canvas->scrolling_mode : false;
}

float ypaint_canvas_scene_min_x(struct ypaint_canvas *canvas) {
  return canvas ? canvas->scene_min_x : 0.0f;
}

float ypaint_canvas_scene_min_y(struct ypaint_canvas *canvas) {
  return canvas ? canvas->scene_min_y : 0.0f;
}

float ypaint_canvas_scene_max_x(struct ypaint_canvas *canvas) {
  return canvas ? canvas->scene_max_x : 0.0f;
}

float ypaint_canvas_scene_max_y(struct ypaint_canvas *canvas) {
  return canvas ? canvas->scene_max_y : 0.0f;
}

float ypaint_canvas_cell_size_x(struct ypaint_canvas *canvas) {
  return canvas ? canvas->cell_size_x : 0.0f;
}

float ypaint_canvas_cell_size_y(struct ypaint_canvas *canvas) {
  return canvas ? canvas->cell_size_y : 0.0f;
}

uint32_t ypaint_canvas_grid_width(struct ypaint_canvas *canvas) {
  return canvas ? canvas->grid_width : 0;
}

uint32_t ypaint_canvas_grid_height(struct ypaint_canvas *canvas) {
  return canvas ? canvas->grid_height : 0;
}

uint32_t ypaint_canvas_max_prims_per_cell(struct ypaint_canvas *canvas) {
  return canvas ? canvas->max_prims_per_cell : 0;
}

uint32_t ypaint_canvas_line_count(struct ypaint_canvas *canvas) {
  return canvas ? canvas->lines.count : 0;
}

uint32_t ypaint_canvas_height_in_lines(struct ypaint_canvas *canvas) {
  if (!canvas || canvas->cell_size_y <= 0.0f)
    return 0;
  float scene_h = canvas->scene_max_y - canvas->scene_min_y;
  return (uint32_t)ceilf(scene_h / canvas->cell_size_y);
}

//=============================================================================
// Cursor
//=============================================================================

void ypaint_canvas_set_cursor(struct ypaint_canvas *canvas, uint16_t col,
                              uint16_t row) {
  if (canvas) {
    canvas->cursor_col = col;
    canvas->cursor_row = row;
  }
}

uint16_t ypaint_canvas_cursor_col(struct ypaint_canvas *canvas) {
  return canvas ? canvas->cursor_col : 0;
}

uint16_t ypaint_canvas_cursor_row(struct ypaint_canvas *canvas) {
  return canvas ? canvas->cursor_row : 0;
}

//=============================================================================
// Rolling offset
//=============================================================================

uint32_t ypaint_canvas_row0_absolute(struct ypaint_canvas *canvas) {
  return canvas ? canvas->row0_absolute : 0;
}

//=============================================================================
// Primitive management
//=============================================================================

static uint32_t cell_x_from_world(struct ypaint_canvas *canvas, float world_x) {
  if (canvas->grid_width == 0 || canvas->cell_size_x <= 0.0f)
    return 0;
  float normalized = (world_x - canvas->scene_min_x) / canvas->cell_size_x;
  if (normalized < 0.0f)
    return 0;
  if (normalized >= (float)canvas->grid_width)
    return canvas->grid_width - 1;
  return (uint32_t)normalized;
}

void ypaint_canvas_add_primitive(struct ypaint_canvas *canvas,
                                 const float *prim_data, uint32_t word_count,
                                 float aabb_min_x, float aabb_min_y,
                                 float aabb_max_x, float aabb_max_y) {
  if (!canvas || !prim_data || word_count == 0 || word_count > 32)
    return;

  // Offset AABB Y by cursor position for grid placement
  float cursor_y_offset = canvas->cursor_row * canvas->cell_size_y;
  aabb_min_y += cursor_y_offset;
  aabb_max_y += cursor_y_offset;

  float base_y = (canvas->scene_min_y > 1e9f) ? 0.0f : canvas->scene_min_y;

  // Row range within primitive's AABB
  int32_t local_min_row =
      (int32_t)floorf((aabb_min_y - base_y) / canvas->cell_size_y);
  int32_t local_max_row =
      (int32_t)floorf((aabb_max_y - base_y) / canvas->cell_size_y);
  if (local_min_row < 0)
    local_min_row = 0;
  if (local_max_row < 0)
    local_max_row = 0;

  uint32_t prim_min_row = (uint32_t)local_min_row;
  uint32_t prim_max_row = (uint32_t)local_max_row;

  // Ensure lines exist
  canvas_ensure_lines(canvas, prim_max_row + 1);

  // Rolling_row at insertion (for shader y_offset calculation)
  uint32_t rolling_row = canvas->row0_absolute + canvas->cursor_row;

  // Store primitive at prim_max_row (bottom of AABB - for scroll deletion)
  // Geometry coordinates stored as-is (no transformation)
  // Shader adjusts test position using y_offset from rolling_row
  struct ypaint_canvas_grid_line *base_line =
      line_buffer_get(&canvas->lines, prim_max_row);
  uint32_t prim_index = prim_data_array_push(&base_line->prims, rolling_row,
                                             prim_data, word_count);

  // Add grid cell references
  uint32_t cell_min_x = cell_x_from_world(canvas, aabb_min_x);
  uint32_t cell_max_x = cell_x_from_world(canvas, aabb_max_x);

  for (uint32_t row = prim_min_row; row <= prim_max_row; row++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, row);
    grid_line_ensure_cells(line, cell_max_x + 1);

    uint16_t lines_ahead = (uint16_t)(prim_max_row - row);
    for (uint32_t cx = cell_min_x; cx <= cell_max_x; cx++) {
      struct ypaint_canvas_prim_ref ref = {lines_ahead, (uint16_t)prim_index};
      prim_ref_array_push(&line->cells[cx].refs, ref);
    }
  }

  canvas->dirty = true;
}

void ypaint_canvas_commit_buffer(struct ypaint_canvas *canvas,
                                 uint32_t max_row) {
  if (!canvas || canvas->grid_height == 0)
    return;

  // Target cursor = row after the buffer's max row
  uint32_t target_cursor = max_row + 1;

  if (target_cursor >= canvas->grid_height) {
    uint16_t lines_to_scroll =
        (uint16_t)(target_cursor - canvas->grid_height + 1);

    // Notify other layers via callback BEFORE scrolling
    if (canvas->scroll_callback)
      canvas->scroll_callback(canvas->scroll_callback_user_data,
                              lines_to_scroll);

    // Scroll ypaint grid
    ypaint_canvas_scroll_lines(canvas, lines_to_scroll);

    // Cursor at last visible row
    canvas->cursor_row = (uint16_t)(canvas->grid_height - 1);
  } else {
    // No scroll needed, just move cursor
    canvas->cursor_row = (uint16_t)target_cursor;
  }
}

//=============================================================================
// Scrolling
//=============================================================================

void ypaint_canvas_scroll_lines(struct ypaint_canvas *canvas,
                                uint16_t num_lines) {
  if (!canvas || num_lines == 0 || canvas->lines.count == 0)
    return;

  // Pop lines from front - primitives in those lines are deleted
  line_buffer_pop_front(&canvas->lines, num_lines);

  // Update row0_absolute to rolling_row of first remaining line
  if (canvas->lines.count > 0) {
    struct ypaint_canvas_grid_line *first = line_buffer_get(&canvas->lines, 0);
    canvas->row0_absolute = first->rolling_row;
  } else {
    canvas->row0_absolute = canvas->next_rolling_row;
  }

  // Update cursor
  if (canvas->cursor_row >= num_lines)
    canvas->cursor_row -= num_lines;
  else
    canvas->cursor_row = 0;

  canvas->dirty = true;
}

void ypaint_canvas_set_scroll_callback(struct ypaint_canvas *canvas,
                                       ypaint_canvas_scroll_callback callback,
                                       void *user_data) {
  if (!canvas)
    return;
  canvas->scroll_callback = callback;
  canvas->scroll_callback_user_data = user_data;
}

//=============================================================================
// Packed GPU format
//=============================================================================

void ypaint_canvas_mark_dirty(struct ypaint_canvas *canvas) {
  if (canvas)
    canvas->dirty = true;
}

bool ypaint_canvas_is_dirty(struct ypaint_canvas *canvas) {
  return canvas ? canvas->dirty : false;
}

static void ensure_grid_staging(struct ypaint_canvas *canvas,
                                uint32_t min_size) {
  if (min_size <= canvas->grid_staging_capacity)
    return;

  uint32_t new_cap = canvas->grid_staging_capacity == 0
                         ? INITIAL_STAGING_CAPACITY
                         : canvas->grid_staging_capacity;
  while (new_cap < min_size)
    new_cap *= 2;

  canvas->grid_staging =
      realloc(canvas->grid_staging, new_cap * sizeof(uint32_t));
  canvas->grid_staging_capacity = new_cap;
}

void ypaint_canvas_rebuild_grid(struct ypaint_canvas *canvas) {
  ypaint_canvas_rebuild_grid_with_glyphs(canvas, 0, NULL, NULL);
}

void ypaint_canvas_rebuild_grid_with_glyphs(
    struct ypaint_canvas *canvas, uint32_t glyph_count,
    ypaint_canvas_glyph_bounds_func bounds_func, void *user_data) {
  if (!canvas)
    return;
  if (!canvas->dirty && canvas->grid_staging_count > 0)
    return;

  // Build line base prim index mapping
  uint32_t total_prims = 0;
  uint32_t *line_base_prim_idx = NULL;
  if (canvas->lines.count > 0) {
    line_base_prim_idx = malloc(canvas->lines.count * sizeof(uint32_t));
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
      line_base_prim_idx[i] = total_prims;
      struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);
      total_prims += line->prims.count;
    }
  }

  // Compute grid dimensions
  uint32_t grid_h = ypaint_canvas_height_in_lines(canvas);
  uint32_t grid_w = 0;
  if (canvas->cell_size_x > 0.0f && canvas->scene_max_x > canvas->scene_min_x)
    grid_w = (uint32_t)ceilf((canvas->scene_max_x - canvas->scene_min_x) /
                             canvas->cell_size_x);

  // Extend for actual lines
  if (canvas->lines.count > grid_h)
    grid_h = canvas->lines.count;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);
    if (line->cell_count > grid_w)
      grid_w = line->cell_count;
  }

  // Extend for glyphs
  if (glyph_count > 0 && bounds_func && canvas->cell_size_x > 0 &&
      canvas->cell_size_y > 0) {
    for (uint32_t gi = 0; gi < glyph_count; gi++) {
      float g_min_x, g_min_y, g_max_x, g_max_y;
      bounds_func(user_data, gi, &g_min_x, &g_min_y, &g_max_x, &g_max_y);
      uint32_t max_cell_x =
          (uint32_t)fmaxf(0.0f, floorf((g_max_x - canvas->scene_min_x) /
                                       canvas->cell_size_x)) +
          1;
      uint32_t max_cell_y =
          (uint32_t)fmaxf(0.0f, floorf((g_max_y - canvas->scene_min_y) /
                                       canvas->cell_size_y)) +
          1;
      if (max_cell_x > grid_w)
        grid_w = max_cell_x;
      if (max_cell_y > grid_h)
        grid_h = max_cell_y;
    }
  }

  canvas->grid_width = grid_w;
  canvas->grid_height = grid_h;

  if (grid_w == 0 || grid_h == 0) {
    canvas->grid_staging_count = 0;
    canvas->dirty = false;
    free(line_base_prim_idx);
    return;
  }

  uint32_t num_cells = grid_w * grid_h;

  // Build glyph->cell mapping
  uint32_t **cell_glyphs = NULL;
  uint32_t *cell_glyph_counts = NULL;
  if (glyph_count > 0 && bounds_func) {
    cell_glyphs = calloc(num_cells, sizeof(uint32_t *));
    cell_glyph_counts = calloc(num_cells, sizeof(uint32_t));

    for (uint32_t gi = 0; gi < glyph_count; gi++) {
      float g_min_x, g_min_y, g_max_x, g_max_y;
      bounds_func(user_data, gi, &g_min_x, &g_min_y, &g_max_x, &g_max_y);

      int32_t c_min_x = (int32_t)floorf((g_min_x - canvas->scene_min_x) /
                                        canvas->cell_size_x);
      int32_t c_min_y = (int32_t)floorf((g_min_y - canvas->scene_min_y) /
                                        canvas->cell_size_y);
      int32_t c_max_x = (int32_t)floorf((g_max_x - canvas->scene_min_x) /
                                        canvas->cell_size_x);
      int32_t c_max_y = (int32_t)floorf((g_max_y - canvas->scene_min_y) /
                                        canvas->cell_size_y);

      if (c_min_x < 0)
        c_min_x = 0;
      if (c_min_y < 0)
        c_min_y = 0;
      if (c_max_x >= (int32_t)grid_w)
        c_max_x = grid_w - 1;
      if (c_max_y >= (int32_t)grid_h)
        c_max_y = grid_h - 1;

      for (int32_t cy = c_min_y; cy <= c_max_y; cy++) {
        for (int32_t cx = c_min_x; cx <= c_max_x; cx++) {
          uint32_t cell_idx = cy * grid_w + cx;
          uint32_t cnt = cell_glyph_counts[cell_idx];
          cell_glyphs[cell_idx] =
              realloc(cell_glyphs[cell_idx], (cnt + 1) * sizeof(uint32_t));
          cell_glyphs[cell_idx][cnt] = gi;
          cell_glyph_counts[cell_idx]++;
        }
      }
    }
  }

  // Build staging: offset table then appended entries
  ensure_grid_staging(canvas, num_cells * 4);
  canvas->grid_staging_count = num_cells;

  for (uint32_t y = 0; y < grid_h; y++) {
    bool has_line = y < canvas->lines.count;
    struct ypaint_canvas_grid_line *line =
        has_line ? line_buffer_get(&canvas->lines, y) : NULL;
    uint32_t line_cell_count = line ? line->cell_count : 0;

    for (uint32_t x = 0; x < grid_w; x++) {
      uint32_t cell_idx = y * grid_w + x;

      ensure_grid_staging(canvas, canvas->grid_staging_count + 2);
      canvas->grid_staging[cell_idx] = canvas->grid_staging_count;

      uint32_t count_pos = canvas->grid_staging_count++;
      ensure_grid_staging(canvas, canvas->grid_staging_count + 1);
      canvas->grid_staging[count_pos] = 0;
      uint32_t count = 0;

      // Add prim entries
      if (has_line && x < line_cell_count) {
        struct ypaint_canvas_grid_cell *cell = &line->cells[x];
        for (uint32_t ri = 0; ri < cell->refs.count; ri++) {
          struct ypaint_canvas_prim_ref *ref = &cell->refs.data[ri];
          uint32_t bl = y + ref->lines_ahead;
          if (bl < canvas->lines.count && line_base_prim_idx) {
            ensure_grid_staging(canvas, canvas->grid_staging_count + 1);
            canvas->grid_staging[canvas->grid_staging_count++] =
                line_base_prim_idx[bl] + ref->prim_index;
            count++;
          }
        }
      }

      // Add glyph entries with GLYPH_BIT
      if (cell_glyphs && cell_glyph_counts) {
        for (uint32_t gi = 0; gi < cell_glyph_counts[cell_idx]; gi++) {
          ensure_grid_staging(canvas, canvas->grid_staging_count + 1);
          canvas->grid_staging[canvas->grid_staging_count++] =
              cell_glyphs[cell_idx][gi] | YPAINT_GLYPH_BIT;
          count++;
        }
      }

      canvas->grid_staging[count_pos] = count;
    }
  }

  // Cleanup
  free(line_base_prim_idx);
  if (cell_glyphs) {
    for (uint32_t i = 0; i < num_cells; i++)
      free(cell_glyphs[i]);
    free(cell_glyphs);
    free(cell_glyph_counts);
  }

  canvas->dirty = false;
}

const uint32_t *ypaint_canvas_grid_data(struct ypaint_canvas *canvas) {
  return canvas ? canvas->grid_staging : NULL;
}

uint32_t ypaint_canvas_grid_word_count(struct ypaint_canvas *canvas) {
  return canvas ? canvas->grid_staging_count : 0;
}

void ypaint_canvas_clear_staging(struct ypaint_canvas *canvas) {
  if (canvas) {
    canvas->grid_staging_count = 0;
    canvas->prim_staging_count = 0;
  }
}

//=============================================================================
// Primitive staging
//=============================================================================

static void ensure_prim_staging(struct ypaint_canvas *canvas,
                                uint32_t min_size) {
  if (min_size <= canvas->prim_staging_capacity)
    return;

  uint32_t new_cap = canvas->prim_staging_capacity == 0
                         ? INITIAL_STAGING_CAPACITY
                         : canvas->prim_staging_capacity;
  while (new_cap < min_size)
    new_cap *= 2;

  canvas->prim_staging =
      realloc(canvas->prim_staging, new_cap * sizeof(uint32_t));
  canvas->prim_staging_capacity = new_cap;
}

const uint32_t *ypaint_canvas_build_prim_staging(struct ypaint_canvas *canvas,
                                                 uint32_t *word_count) {
  if (!canvas) {
    if (word_count)
      *word_count = 0;
    return NULL;
  }

  // Count primitives and total words (+1 per prim for rolling_row)
  uint32_t prim_count = 0;
  uint32_t total_words = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);
    for (uint32_t p = 0; p < line->prims.count; p++) {
      prim_count++;
      total_words += line->prims.data[p].word_count + 1; // +1 for rolling_row
    }
  }

  if (prim_count == 0) {
    canvas->prim_staging_count = 0;
    if (word_count)
      *word_count = 0;
    return NULL;
  }

  // Layout: [prim0_offset, prim1_offset, ...][rolling_row0,
  // prim0_data...][rolling_row1, prim1_data...]
  uint32_t total_size = prim_count + total_words;
  ensure_prim_staging(canvas, total_size);

  uint32_t data_offset = 0;
  uint32_t prim_idx = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);

    for (uint32_t p = 0; p < line->prims.count; p++) {
      struct ypaint_canvas_prim_data *prim = &line->prims.data[p];
      canvas->prim_staging[prim_idx] = data_offset;

      // Prepend rolling_row at insertion (for shader y_offset calculation)
      canvas->prim_staging[prim_count + data_offset] = prim->rolling_row;

      // Copy primitive data
      for (uint32_t w = 0; w < prim->word_count; w++) {
        uint32_t val;
        memcpy(&val, &prim->data[w], sizeof(uint32_t));
        canvas->prim_staging[prim_count + data_offset + 1 + w] = val;
      }

      data_offset += prim->word_count + 1; // +1 for rolling_row
      prim_idx++;
    }
  }

  canvas->prim_staging_count = total_size;
  if (word_count)
    *word_count = total_size;
  return canvas->prim_staging;
}

uint32_t ypaint_canvas_prim_gpu_size(struct ypaint_canvas *canvas) {
  if (!canvas)
    return 0;

  uint32_t total_words = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);
    for (uint32_t p = 0; p < line->prims.count; p++)
      total_words += line->prims.data[p].word_count + 1; // +1 for rolling_row
  }
  return total_words * sizeof(float);
}

//=============================================================================
// State management
//=============================================================================

void ypaint_canvas_clear(struct ypaint_canvas *canvas) {
  if (!canvas)
    return;

  line_buffer_free(&canvas->lines);
  line_buffer_init(&canvas->lines);

  canvas->grid_staging_count = 0;
  canvas->prim_staging_count = 0;
  canvas->cursor_col = 0;
  canvas->cursor_row = 0;
  canvas->row0_absolute = 0;
  canvas->next_rolling_row = 0;
  canvas->dirty = true;
}

bool ypaint_canvas_empty(struct ypaint_canvas *canvas) {
  if (!canvas)
    return true;

  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);
    if (line->prims.count > 0)
      return false;
  }
  return true;
}

uint32_t ypaint_canvas_primitive_count(struct ypaint_canvas *canvas) {
  if (!canvas)
    return 0;

  uint32_t count = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct ypaint_canvas_grid_line *line = line_buffer_get(&canvas->lines, i);
    count += line->prims.count;
  }
  return count;
}
