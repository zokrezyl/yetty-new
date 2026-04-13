// YPaint Canvas - Implementation
// Rolling offset approach for O(1) scrolling

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ypaint/core/ypaint-canvas.h>
#include <yetty/ysdf/types.gen.h>
#include <yetty/ytrace.h>

//=============================================================================
// Internal data structures
//=============================================================================

// Reference to a primitive in another line
struct yetty_yetty_ypaint_canvas_prim_ref {
  uint16_t lines_ahead; // relative offset to base line (0 = same line)
  uint16_t prim_index;  // index within base line's prims array
};

// Dynamic array of prim_ref
struct yetty_yetty_ypaint_canvas_prim_ref_array {
  struct yetty_yetty_ypaint_canvas_prim_ref *data;
  uint32_t count;
  uint32_t capacity;
};

// A single grid cell
struct yetty_yetty_ypaint_canvas_grid_cell {
  struct yetty_yetty_ypaint_canvas_prim_ref_array refs;
};

// A single primitive's data
struct yetty_yetty_ypaint_canvas_prim_data {
  uint32_t rolling_row; // rolling_row at insertion (cursor row or explicit)
  float *data;
  uint32_t word_count;
};

// Dynamic array of prim_data
struct yetty_yetty_ypaint_canvas_prim_data_array {
  struct yetty_yetty_ypaint_canvas_prim_data *data;
  uint32_t count;
  uint32_t capacity;
};

// A single row/line in the grid
struct yetty_yetty_ypaint_canvas_grid_line {
  struct yetty_yetty_ypaint_canvas_prim_data_array
      prims; // primitives whose BASE is this line
  struct yetty_yetty_ypaint_canvas_grid_cell *cells; // grid cells for this line
  uint32_t cell_count;
  uint32_t cell_capacity;
};

// Simple line array
struct yetty_yetty_ypaint_canvas_line_buffer {
  struct yetty_yetty_ypaint_canvas_grid_line *lines;
  uint32_t capacity;
  uint32_t count;
};

// Canvas structure
struct yetty_yetty_ypaint_canvas {
  bool scrolling_mode;

  struct pixel_size cell_size;
  struct grid_size grid_size;

  // Cursor (scrolling mode)
  uint16_t cursor_col;
  uint16_t cursor_row;

  // Rolling row of visible line 0 (increments on scroll)
  uint32_t rolling_row_0;

  // Lines
  struct yetty_yetty_ypaint_canvas_line_buffer lines;

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
  yetty_yetty_ypaint_canvas_scroll_callback scroll_callback;
  struct yetty_core_void_result *scroll_callback_user_data;

  // Cursor set callback (when cursor moves without scroll)
  yetty_yetty_ypaint_canvas_cursor_set_callback cursor_set_callback;
  struct yetty_core_void_result *cursor_set_callback_user_data;
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

static void
prim_ref_array_init(struct yetty_yetty_ypaint_canvas_prim_ref_array *arr) {
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static void
prim_ref_array_free(struct yetty_yetty_ypaint_canvas_prim_ref_array *arr) {
  free(arr->data);
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static void
prim_ref_array_push(struct yetty_yetty_ypaint_canvas_prim_ref_array *arr,
                    struct yetty_yetty_ypaint_canvas_prim_ref ref) {
  if (arr->count >= arr->capacity) {
    uint32_t new_cap =
        arr->capacity == 0 ? INITIAL_REF_CAPACITY : arr->capacity * 2;
    arr->data = realloc(
        arr->data, new_cap * sizeof(struct yetty_yetty_ypaint_canvas_prim_ref));
    arr->capacity = new_cap;
  }
  arr->data[arr->count++] = ref;
}

static void
prim_data_array_init(struct yetty_yetty_ypaint_canvas_prim_data_array *arr) {
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static void
prim_data_array_free(struct yetty_yetty_ypaint_canvas_prim_data_array *arr) {
  for (uint32_t i = 0; i < arr->count; i++)
    free(arr->data[i].data);
  free(arr->data);
  arr->data = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

static uint32_t
prim_data_array_push(struct yetty_yetty_ypaint_canvas_prim_data_array *arr,
                     uint32_t rolling_row, const float *data,
                     uint32_t word_count) {
  if (arr->count >= arr->capacity) {
    uint32_t new_cap =
        arr->capacity == 0 ? INITIAL_PRIM_CAPACITY : arr->capacity * 2;
    arr->data =
        realloc(arr->data,
                new_cap * sizeof(struct yetty_yetty_ypaint_canvas_prim_data));
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

static struct yetty_core_void_result
grid_line_init(struct yetty_yetty_ypaint_canvas_grid_line *line,
               uint32_t initial_cells) {
  prim_data_array_init(&line->prims);
  line->cells = NULL;
  line->cell_count = 0;
  line->cell_capacity = 0;
  if (initial_cells > 0) {
    line->cells = calloc(initial_cells,
                         sizeof(struct yetty_yetty_ypaint_canvas_grid_cell));
    if (!line->cells)
      return YETTY_ERR(yetty_core_void, "calloc failed for grid cells");
    line->cell_capacity = initial_cells;
  }
  return YETTY_OK_VOID();
}

static void grid_line_free(struct yetty_yetty_ypaint_canvas_grid_line *line) {
  prim_data_array_free(&line->prims);
  for (uint32_t i = 0; i < line->cell_count; i++)
    prim_ref_array_free(&line->cells[i].refs);
  free(line->cells);
  line->cells = NULL;
  line->cell_count = 0;
  line->cell_capacity = 0;
}

static struct yetty_core_void_result
grid_line_ensure_cells(struct yetty_yetty_ypaint_canvas_grid_line *line,
                       uint32_t min_cells) {
  if (min_cells <= line->cell_capacity) {
    if (min_cells > line->cell_count) {
      for (uint32_t i = line->cell_count; i < min_cells; i++)
        prim_ref_array_init(&line->cells[i].refs);
      line->cell_count = min_cells;
    }
    return YETTY_OK_VOID();
  }

  uint32_t new_cap =
      line->cell_capacity == 0 ? INITIAL_CELL_CAPACITY : line->cell_capacity;
  while (new_cap < min_cells)
    new_cap *= 2;

  struct yetty_yetty_ypaint_canvas_grid_cell *new_cells =
      realloc(line->cells,
              new_cap * sizeof(struct yetty_yetty_ypaint_canvas_grid_cell));
  if (!new_cells)
    return YETTY_ERR(yetty_core_void, "realloc failed for grid cells");
  line->cells = new_cells;
  for (uint32_t i = line->cell_capacity; i < new_cap; i++)
    prim_ref_array_init(&line->cells[i].refs);
  line->cell_capacity = new_cap;
  line->cell_count = min_cells;
  return YETTY_OK_VOID();
}

//=============================================================================
// Helper: line_buffer (circular buffer)
//=============================================================================

static void
line_buffer_init(struct yetty_yetty_ypaint_canvas_line_buffer *buf) {
  buf->lines = NULL;
  buf->capacity = 0;
  buf->count = 0;
}

static void
line_buffer_free(struct yetty_yetty_ypaint_canvas_line_buffer *buf) {
  for (uint32_t i = 0; i < buf->count; i++) {
    grid_line_free(&buf->lines[i]);
  }
  free(buf->lines);
  buf->lines = NULL;
  buf->capacity = 0;
  buf->count = 0;
}

static struct yetty_yetty_ypaint_canvas_grid_line *
line_buffer_get(struct yetty_yetty_ypaint_canvas_line_buffer *buf,
                uint32_t index) {
  if (index >= buf->count)
    return NULL;
  return &buf->lines[index];
}

static struct yetty_core_void_result
canvas_ensure_lines(struct yetty_yetty_ypaint_canvas *canvas,
                    uint32_t min_count) {
  struct yetty_yetty_ypaint_canvas_line_buffer *buf = &canvas->lines;

  // Grow capacity if needed
  if (min_count > buf->capacity) {
    uint32_t new_cap =
        buf->capacity == 0 ? INITIAL_LINE_CAPACITY : buf->capacity;
    while (new_cap < min_count)
      new_cap *= 2;

    struct yetty_yetty_ypaint_canvas_grid_line *new_lines =
        realloc(buf->lines,
                new_cap * sizeof(struct yetty_yetty_ypaint_canvas_grid_line));
    if (!new_lines)
      return YETTY_ERR(yetty_core_void, "realloc failed for line buffer");
    buf->lines = new_lines;
    buf->capacity = new_cap;
  }

  // Initialize new lines at the end
  while (buf->count < min_count) {
    struct yetty_core_void_result r =
        grid_line_init(&buf->lines[buf->count], canvas->grid_size.cols);
    if (!r.ok)
      return r;
    buf->count++;
  }
  return YETTY_OK_VOID();
}

static void
line_buffer_pop_front(struct yetty_yetty_ypaint_canvas_line_buffer *buf,
                      uint32_t count) {
  if (count == 0 || buf->count == 0)
    return;
  if (count > buf->count)
    count = buf->count;

  // Free the top lines being removed
  for (uint32_t i = 0; i < count; i++) {
    grid_line_free(&buf->lines[i]);
  }

  // Memmove remaining lines to front
  uint32_t remaining = buf->count - count;
  if (remaining > 0) {
    memmove(buf->lines, buf->lines + count,
            remaining * sizeof(struct yetty_yetty_ypaint_canvas_grid_line));
  }

  // Zero the freed slots at the end
  memset(buf->lines + remaining, 0,
         count * sizeof(struct yetty_yetty_ypaint_canvas_grid_line));

  buf->count = remaining;
}

//=============================================================================
// Canvas implementation
//=============================================================================

struct yetty_yetty_ypaint_canvas *
yetty_yetty_ypaint_canvas_create(bool scrolling_mode) {
  struct yetty_yetty_ypaint_canvas *canvas;

  canvas = calloc(1, sizeof(struct yetty_yetty_ypaint_canvas));
  if (!canvas)
    return NULL;

  canvas->scrolling_mode = scrolling_mode;
  canvas->dirty = true;
  canvas->rolling_row_0 = 0;

  line_buffer_init(&canvas->lines);

  return canvas;
}

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_destroy(struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");

  line_buffer_free(&canvas->lines);
  free(canvas->grid_staging);
  free(canvas->prim_staging);
  free(canvas);
  return YETTY_OK_VOID();
}

//=============================================================================
// Configuration
//=============================================================================

struct yetty_core_void_result yetty_yetty_ypaint_canvas_set_cell_size(
    struct yetty_yetty_ypaint_canvas *canvas, struct pixel_size size) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  if (size.width <= 0.0f || size.height <= 0.0f)
    return YETTY_ERR(yetty_core_void, "cell size must be > 0");
  canvas->cell_size = size;
  canvas->dirty = true;
  return YETTY_OK_VOID();
}

struct yetty_core_void_result yetty_yetty_ypaint_canvas_set_grid_size(
    struct yetty_yetty_ypaint_canvas *canvas, struct grid_size size) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  canvas->grid_size = size;
  canvas->dirty = true;
  return YETTY_OK_VOID();
}

//=============================================================================
// Accessors
//=============================================================================

struct pixel_size yetty_yetty_ypaint_canvas_cell_get_pixel_size(
    struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return (struct pixel_size){0, 0};
  return canvas->cell_size;
}

struct grid_size yetty_yetty_ypaint_canvas_get_grid_size(
    struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return (struct grid_size){0, 0};
  return canvas->grid_size;
}

//=============================================================================
// Cursor
//=============================================================================

struct yetty_core_void_result yetty_yetty_ypaint_canvas_set_cursor_pos(
    struct yetty_yetty_ypaint_canvas *canvas, struct grid_cursor_pos pos) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  canvas->cursor_col = pos.cols;
  canvas->cursor_row = pos.rows;
  return YETTY_OK_VOID();
}

uint16_t
yetty_yetty_ypaint_canvas_cursor_col(struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->cursor_col : 0;
}

uint16_t
yetty_yetty_ypaint_canvas_cursor_row(struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->cursor_row : 0;
}

//=============================================================================
// Rolling offset
//=============================================================================

uint32_t yetty_yetty_ypaint_canvas_rolling_row_0(
    struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->rolling_row_0 : 0;
}

//=============================================================================
// Primitive management
//=============================================================================

// Add a single primitive (internal)
// Returns the max_row (bottom row of AABB) for this primitive
static uint32_t add_primitive_internal(
    struct yetty_yetty_ypaint_canvas *canvas,
    const struct yetty_ypaint_core_primitive_iter *iter) {
  if (!canvas || !iter || !iter->data || iter->size == 0)
    return 0;

  // Validate cell_height to avoid division issues
  if (canvas->cell_size.height <= 0.0f) {
    yerror("BUG: cell_height=%.1f <= 0, cannot compute rows!",
           canvas->cell_size.height);
    return 0;
  }

  uint32_t word_count = iter->size / sizeof(float);
  float aabb_min_x, aabb_min_y, aabb_max_x, aabb_max_y;
  yetty_ysdf_compute_aabb(iter->data, word_count, &aabb_min_x, &aabb_min_y,
                          &aabb_max_x, &aabb_max_y);

  // Check for inverted AABB (bug in compute_aabb)
  if (aabb_min_y > aabb_max_y) {
    yerror("BUG: inverted AABB! aabb_min_y=%.1f > aabb_max_y=%.1f", aabb_min_y,
           aabb_max_y);
    // Swap to fix
    float tmp = aabb_min_y;
    aabb_min_y = aabb_max_y;
    aabb_max_y = tmp;
  }

  // Offset AABB Y by cursor position for grid placement
  float cursor_y_offset = canvas->cursor_row * canvas->cell_size.height;
  aabb_min_y += cursor_y_offset;
  aabb_max_y += cursor_y_offset;

  // Row range within primitive's AABB
  int32_t local_min_row =
      (int32_t)floorf(aabb_min_y / canvas->cell_size.height);
  int32_t local_max_row =
      (int32_t)floorf(aabb_max_y / canvas->cell_size.height);
  if (local_min_row < 0)
    local_min_row = 0;
  if (local_max_row < 0)
    local_max_row = 0;

  uint32_t prim_min_row = (uint32_t)local_min_row;
  uint32_t prim_max_row = (uint32_t)local_max_row;

  // BUG CHECK: if min > max, AABB is inverted (bug in compute_aabb or cell_size
  // issue)
  if (prim_min_row > prim_max_row) {
    yerror("BUG: prim_min_row=%u > prim_max_row=%u! aabb_y=[%.1f,%.1f] "
           "cell_height=%.1f",
           prim_min_row, prim_max_row, aabb_min_y, aabb_max_y,
           canvas->cell_size.height);
    // Swap to avoid crash, but this is a BUG that needs fixing
    uint32_t tmp = prim_min_row;
    prim_min_row = prim_max_row;
    prim_max_row = tmp;
  }

  // Ensure lines exist
  canvas_ensure_lines(canvas, prim_max_row + 1);

  // rolling_row = rolling_row_0 + cursor_row
  // Encodes absolute cursor position at insertion time for shader y_offset
  uint32_t rolling_row = canvas->rolling_row_0 + canvas->cursor_row;

  // Store primitive at prim_max_row (bottom of AABB - for scroll deletion)
  // Geometry coordinates stored as-is (no transformation)
  // Shader adjusts test position using y_offset from rolling_row
  struct yetty_yetty_ypaint_canvas_grid_line *base_line =
      line_buffer_get(&canvas->lines, rolling_row);

  uint32_t prim_index = prim_data_array_push(&base_line->prims, rolling_row,
                                             iter->data, word_count);

  // Add grid cell references (convert pixel x to column)
  uint32_t col_min = (canvas->cell_size.width > 0)
                         ? (uint32_t)(aabb_min_x / canvas->cell_size.width)
                         : 0;
  uint32_t col_max = (canvas->cell_size.width > 0)
                         ? (uint32_t)(aabb_max_x / canvas->cell_size.width)
                         : 0;
  if (col_max >= canvas->grid_size.cols && canvas->grid_size.cols > 0)
    col_max = canvas->grid_size.cols - 1;

  for (uint32_t row = prim_min_row; row <= prim_max_row; row++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, row);
    grid_line_ensure_cells(line, col_max + 1);

    uint16_t lines_ahead = (uint16_t)(prim_max_row - row);
    for (uint32_t col = col_min; col <= col_max; col++) {
      struct yetty_yetty_ypaint_canvas_prim_ref ref = {lines_ahead,
                                                       (uint16_t)prim_index};
      prim_ref_array_push(&line->cells[col].refs, ref);
    }
  }

  ydebug("add_primitive_internal: aabb_y=[%.1f,%.1f] cell_height=%.1f "
         "cursor_row=%u",
         aabb_min_y, aabb_max_y, canvas->cell_size.height, canvas->cursor_row);
  ydebug(
      "add_primitive_internal: prim_min_row=%u prim_max_row=%u lines.count=%u",
      prim_min_row, prim_max_row, canvas->lines.count);

  canvas->dirty = true;
  return rolling_row;
}

// Commit buffer after adding all primitives (internal)
// Handles auto-scroll if primitives extend beyond visible area
struct yetty_core_void_result yetty_yetty_ypaint_canvas_commit_buffer_internal(
    struct yetty_yetty_ypaint_canvas *canvas, uint32_t max_row) {
  if (!canvas) {
    yerror("yetty_yetty_ypaint_canvas_commit_buffer_internal: canvas is NULL");
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  }
  if (canvas->grid_size.rows == 0) {
    yerror("yetty_yetty_ypaint_canvas_commit_buffer_internal: grid_rows is 0, "
           "scene "
           "bounds not set?");
    return YETTY_ERR(yetty_core_void, "grid_rows is 0");
  }

  ydebug("yetty_yetty_ypaint_canvas_commit_buffer_internal: max_row=%u "
         "cursor_row=%u "
         "grid_rows=%u",
         max_row, canvas->cursor_row, canvas->grid_size.rows);

  // Target cursor = row after the buffer's max row
  uint32_t target_cursor_row = max_row + 1;

  if (target_cursor_row >= canvas->grid_size.rows) {
    uint16_t lines_to_scroll =
        (uint16_t)(target_cursor_row - canvas->grid_size.rows + 1);

    ydebug(
        "yetty_yetty_ypaint_canvas_commit_buffer_internal: SCROLL target=%u >= "
        "grid_rows=%u, scroll %u lines",
        target_cursor_row, canvas->grid_size.rows, lines_to_scroll);

    // Notify other layers via callback BEFORE scrolling
    if (!canvas->scroll_callback) {
      yerror("yetty_yetty_ypaint_canvas_commit_buffer_internal: "
             "scroll_callback is "
             "NULL");
      return YETTY_ERR(yetty_core_void, "scroll_callback is NULL");
    }
    struct yetty_core_void_result scroll_res = canvas->scroll_callback(
        canvas->scroll_callback_user_data, lines_to_scroll);
    if (YETTY_IS_ERR(scroll_res)) {
      yerror("yetty_yetty_ypaint_canvas_commit_buffer_internal: "
             "scroll_callback failed");
      return scroll_res;
    }

    // Scroll ypaint grid
    yetty_yetty_ypaint_canvas_scroll_lines(canvas, lines_to_scroll);

    // Cursor at last visible row
    canvas->cursor_row = (uint16_t)(canvas->grid_size.rows - 1);
    ydebug("yetty_yetty_ypaint_canvas_commit_buffer_internal: cursor_row set "
           "to %u "
           "(last "
           "visible)",
           canvas->cursor_row);
  } else {
    // No scroll needed, just move cursor
    uint16_t new_row = (uint16_t)target_cursor_row;

    ydebug("yetty_yetty_ypaint_canvas_commit_buffer_internal: CURSOR MOVE "
           "target=%u < "
           "grid_rows=%u, new_row=%u",
           target_cursor_row, canvas->grid_size.rows, new_row);

    canvas->cursor_row = new_row;

    // Notify other layers of cursor position change
    if (!canvas->cursor_set_callback) {
      yerror("yetty_yetty_ypaint_canvas_commit_buffer_internal: "
             "cursor_set_callback "
             "is NULL");
      return YETTY_ERR(yetty_core_void, "cursor_set_callback is NULL");
    }
    struct yetty_core_void_result cursor_res = canvas->cursor_set_callback(
        canvas->cursor_set_callback_user_data, new_row);
    if (YETTY_IS_ERR(cursor_res)) {
      yerror("yetty_yetty_ypaint_canvas_commit_buffer_internal: "
             "cursor_set_callback "
             "failed");
      return cursor_res;
    }
    ydebug(
        "yetty_yetty_ypaint_canvas_commit_buffer_internal: cursor_set_callback "
        "succeeded");
  }

  return YETTY_OK_VOID();
}

//=============================================================================
// Buffer management (public API)
//=============================================================================

struct yetty_core_void_result
yetty_ypaint_canvas_add_buffer(struct yetty_yetty_ypaint_canvas *canvas,
                               struct yetty_ypaint_core_buffer *buffer) {
  if (!canvas) {
    yerror("yetty_ypaint_canvas_add_buffer: canvas is NULL");
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  }
  if (!buffer) {
    yerror("yetty_ypaint_canvas_add_buffer: buffer is NULL");
    return YETTY_ERR(yetty_core_void, "buffer is NULL");
  }

  // Check if buffer is empty
  struct yetty_ypaint_core_primitive_iter_result iter_res =
      yetty_ypaint_core_buffer_prim_first(buffer);
  if (YETTY_IS_ERR(iter_res))
    return YETTY_OK_VOID(); // Empty buffer is OK

  // Save original cursor - primitives coords are relative to THIS position
  uint16_t original_cursor_row = canvas->cursor_row;
  uint32_t original_rolling_row_0 = canvas->rolling_row_0;
  (void)original_rolling_row_0;

  ydebug("add_buffer: START cursor_row=%u grid_rows=%u rolling_row_0=%u",
         canvas->cursor_row, canvas->grid_size.rows, canvas->rolling_row_0);

  // PASS 1: Compute max_row needed WITHOUT adding primitives
  uint32_t max_row_needed = 0;
  struct yetty_ypaint_core_primitive_iter iter = iter_res.value;

  while (1) {
    uint32_t word_count = iter.size / sizeof(float);

    // Compute AABB to find max row (using ORIGINAL cursor position)
    float aabb_min_x, aabb_min_y, aabb_max_x, aabb_max_y;
    yetty_ysdf_compute_aabb(iter.data, word_count, &aabb_min_x, &aabb_min_y,
                            &aabb_max_x, &aabb_max_y);

    float cursor_y_offset = original_cursor_row * canvas->cell_size.height;
    float abs_max_y = aabb_max_y + cursor_y_offset;
    uint32_t prim_max_row =
        (canvas->cell_size.height > 0)
            ? (uint32_t)floorf(abs_max_y / canvas->cell_size.height)
            : 0;

    if (prim_max_row > max_row_needed)
      max_row_needed = prim_max_row;

    iter_res = yetty_ypaint_core_buffer_prim_next(buffer, &iter);
    if (YETTY_IS_ERR(iter_res))
      break;
    iter = iter_res.value;
  }

  ydebug("add_buffer: PASS1 max_row_needed=%u (cursor-relative to row %u)",
         max_row_needed, original_cursor_row);

  // SCROLL FIRST if primitives would extend beyond visible area
  uint16_t lines_scrolled = 0;
  uint32_t target_cursor_row = max_row_needed + 1;
  if (target_cursor_row >= canvas->grid_size.rows) {
    lines_scrolled = (uint16_t)(target_cursor_row - canvas->grid_size.rows + 1);

    ydebug("add_buffer: SCROLL NEEDED target=%u >= grid_rows=%u, scroll %u",
           target_cursor_row, canvas->grid_size.rows, lines_scrolled);

    // Notify text layer BEFORE scrolling
    if (!canvas->scroll_callback) {
      yerror("yetty_ypaint_canvas_add_buffer: scroll_callback is NULL");
      return YETTY_ERR(yetty_core_void, "scroll_callback is NULL");
    }
    struct yetty_core_void_result scroll_res = canvas->scroll_callback(
        canvas->scroll_callback_user_data, lines_scrolled);
    if (YETTY_IS_ERR(scroll_res)) {
      yerror("yetty_ypaint_canvas_add_buffer: scroll_callback failed");
      return scroll_res;
    }

    // Scroll ypaint grid (this updates rolling_row_0 and cursor_row)
    yetty_yetty_ypaint_canvas_scroll_lines(canvas, lines_scrolled);

    ydebug("add_buffer: after scroll cursor_row=%u rolling_row_0=%u",
           canvas->cursor_row, canvas->rolling_row_0);
  }

  // PASS 2: Add primitives
  // Use ADJUSTED cursor = original - lines_scrolled (where prims should land)
  uint16_t adjusted_cursor = (original_cursor_row >= lines_scrolled)
                                 ? (original_cursor_row - lines_scrolled)
                                 : 0;
  canvas->cursor_row = adjusted_cursor;

  ydebug("add_buffer: PASS2 using adjusted cursor_row=%u (original=%u - "
         "scrolled=%u)",
         adjusted_cursor, original_cursor_row, lines_scrolled);

  uint32_t max_row_seen = 0;

  // Restart iteration for pass 2
  iter_res = yetty_ypaint_core_buffer_prim_first(buffer);
  if (YETTY_IS_ERR(iter_res))
    return YETTY_OK_VOID();
  iter = iter_res.value;

  while (1) {
    uint32_t prim_max_row = add_primitive_internal(canvas, &iter);

    ydebug("add_buffer: PASS2 added prim type=%u max_row=%u", iter.type,
           prim_max_row);

    if (prim_max_row > max_row_seen)
      max_row_seen = prim_max_row;

    iter_res = yetty_ypaint_core_buffer_prim_next(buffer, &iter);
    if (YETTY_IS_ERR(iter_res))
      break;
    iter = iter_res.value;
  }

  // Set final cursor position = row after max primitive row
  uint32_t final_cursor = max_row_seen + 1;
  if (final_cursor >= canvas->grid_size.rows) {
    // Shouldn't happen since we scrolled, but clamp just in case
    final_cursor = canvas->grid_size.rows - 1;
  }
  canvas->cursor_row = (uint16_t)final_cursor;

  // Notify text layer of final cursor position
  if (canvas->cursor_set_callback) {
    canvas->cursor_set_callback(canvas->cursor_set_callback_user_data,
                                canvas->cursor_row);
  }

  ydebug("add_buffer: END cursor_row=%u max_row_seen=%u rolling_row_0=%u",
         canvas->cursor_row, max_row_seen, canvas->rolling_row_0);

  canvas->dirty = true;
  return YETTY_OK_VOID();
}

//=============================================================================
// Scrolling
//=============================================================================

static struct yetty_core_void_result
dump_grid(struct yetty_yetty_ypaint_canvas *canvas, const char *label) {
  ydebug("=== GRID DUMP [%s] lines.count=%u rolling_row_0=%u grid=%ux%u "
         "staging=%u ===",
         label, canvas->lines.count, canvas->rolling_row_0,
         canvas->grid_size.cols, canvas->grid_size.rows,
         canvas->grid_staging_count);
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    uint32_t total_refs = 0;
    uint32_t first_ref_col = 0xFFFFFFFF;
    for (uint32_t c = 0; c < line->cell_count; c++) {
      total_refs += line->cells[c].refs.count;
      if (line->cells[c].refs.count > 0 && first_ref_col == 0xFFFFFFFF)
        first_ref_col = c;
    }
    if (total_refs > 0 && first_ref_col != 0xFFFFFFFF) {
      struct yetty_yetty_ypaint_canvas_prim_ref *ref =
          &line->cells[first_ref_col].refs.data[0];
      ydebug("  line[%u] prims=%u refs=%u cells=%u first_ref@col%u: ahead=%u "
             "idx=%u",
             i, line->prims.count, total_refs, line->cell_count, first_ref_col,
             ref->lines_ahead, ref->prim_index);
    } else {
      ydebug("  line[%u] prims=%u refs=%u cells=%u", i, line->prims.count,
             total_refs, line->cell_count);
    }
  }
  ydebug("=== END GRID DUMP ===");
  return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_scroll_lines(struct yetty_yetty_ypaint_canvas *canvas,
                                       uint16_t num_lines) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  if (num_lines == 0)
    return YETTY_OK_VOID();

  ydebug("yetty_yetty_ypaint_canvas_scroll_lines: num_lines=%u lines.count=%u "
         "rolling_row_0=%u",
         num_lines, canvas->lines.count, canvas->rolling_row_0);

  dump_grid(canvas, "BEFORE SCROLL");

  // Pop lines from front (memmove)
  line_buffer_pop_front(&canvas->lines, num_lines);

  // Always increment rolling_row_0
  canvas->rolling_row_0 += num_lines;

  // Update cursor
  if (canvas->cursor_row >= num_lines)
    canvas->cursor_row -= num_lines;
  else
    canvas->cursor_row = 0;

  ydebug("yetty_yetty_ypaint_canvas_scroll_lines: after scroll lines.count=%u "
         "rolling_row_0=%u cursor_row=%u",
         canvas->lines.count, canvas->rolling_row_0, canvas->cursor_row);

  dump_grid(canvas, "AFTER SCROLL");

  canvas->dirty = true;
  return YETTY_OK_VOID();
}

struct yetty_core_void_result yetty_yetty_ypaint_canvas_set_scroll_callback(
    struct yetty_yetty_ypaint_canvas *canvas,
    yetty_yetty_ypaint_canvas_scroll_callback callback,
    struct yetty_core_void_result *user_data) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  canvas->scroll_callback = callback;
  canvas->scroll_callback_user_data = user_data;
  return YETTY_OK_VOID();
}

struct yetty_core_void_result yetty_yetty_ypaint_canvas_set_cursor_callback(
    struct yetty_yetty_ypaint_canvas *canvas,
    yetty_yetty_ypaint_canvas_cursor_set_callback callback,
    struct yetty_core_void_result *user_data) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  canvas->cursor_set_callback = callback;
  canvas->cursor_set_callback_user_data = user_data;
  return YETTY_OK_VOID();
}

//=============================================================================
// Packed GPU format
//=============================================================================

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_mark_dirty(struct yetty_yetty_ypaint_canvas *canvas) {
  if (canvas)
    canvas->dirty = true;
  return YETTY_OK_VOID();
}

bool yetty_yetty_ypaint_canvas_is_dirty(
    struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->dirty : false;
}

static struct yetty_core_void_result
ensure_grid_staging(struct yetty_yetty_ypaint_canvas *canvas,
                    uint32_t min_size) {
  if (min_size <= canvas->grid_staging_capacity)
    return YETTY_OK_VOID();

  uint32_t new_cap = canvas->grid_staging_capacity == 0
                         ? INITIAL_STAGING_CAPACITY
                         : canvas->grid_staging_capacity;
  while (new_cap < min_size)
    new_cap *= 2;

  uint32_t *new_staging =
      realloc(canvas->grid_staging, new_cap * sizeof(uint32_t));
  if (!new_staging)
    return YETTY_ERR(yetty_core_void, "realloc failed for grid staging");
  canvas->grid_staging = new_staging;
  canvas->grid_staging_capacity = new_cap;
  return YETTY_OK_VOID();
}

struct yetty_core_void_result yetty_yetty_ypaint_canvas_rebuild_grid(
    struct yetty_yetty_ypaint_canvas *canvas) {
  return yetty_yetty_ypaint_canvas_rebuild_grid_with_glyphs(canvas, 0, NULL,
                                                            NULL);
}

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_rebuild_grid_with_glyphs(
    struct yetty_yetty_ypaint_canvas *canvas, uint32_t glyph_count,
    yetty_yetty_ypaint_canvas_glyph_bounds_func bounds_func,
    struct yetty_core_void_result *user_data) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  if (!canvas->dirty && canvas->grid_staging_count > 0)
    return YETTY_OK_VOID();

  // Build line base prim index mapping
  uint32_t total_prims = 0;
  uint32_t *line_base_prim_idx = NULL;
  if (canvas->lines.count > 0) {
    line_base_prim_idx = malloc(canvas->lines.count * sizeof(uint32_t));
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
      line_base_prim_idx[i] = total_prims;
      struct yetty_yetty_ypaint_canvas_grid_line *line =
          line_buffer_get(&canvas->lines, i);
      total_prims += line->prims.count;
    }
  }

  // Grid dimensions from canvas->grid_size
  uint32_t grid_w = canvas->grid_size.cols;
  uint32_t grid_h = canvas->grid_size.rows;

  // Extend for actual lines if needed
  if (canvas->lines.count > grid_h)
    grid_h = canvas->lines.count;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    if (line->cell_count > grid_w)
      grid_w = line->cell_count;
  }

  // Extend for glyphs if needed
  if (glyph_count > 0 && bounds_func && canvas->cell_size.width > 0 &&
      canvas->cell_size.height > 0) {
    for (uint32_t gi = 0; gi < glyph_count; gi++) {
      float g_min_x, g_min_y, g_max_x, g_max_y;
      bounds_func(user_data, gi, &g_min_x, &g_min_y, &g_max_x, &g_max_y);
      uint32_t max_cell_x =
          (uint32_t)fmaxf(0.0f, floorf(g_max_x / canvas->cell_size.width)) + 1;
      uint32_t max_cell_y =
          (uint32_t)fmaxf(0.0f, floorf(g_max_y / canvas->cell_size.height)) + 1;
      if (max_cell_x > grid_w)
        grid_w = max_cell_x;
      if (max_cell_y > grid_h)
        grid_h = max_cell_y;
    }
  }

  if (grid_w == 0 || grid_h == 0) {
    canvas->grid_staging_count = 0;
    canvas->dirty = false;
    free(line_base_prim_idx);
    return YETTY_OK_VOID();
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

      int32_t c_min_x = (int32_t)floorf(g_min_x / canvas->cell_size.width);
      int32_t c_min_y = (int32_t)floorf(g_min_y / canvas->cell_size.height);
      int32_t c_max_x = (int32_t)floorf(g_max_x / canvas->cell_size.width);
      int32_t c_max_y = (int32_t)floorf(g_max_y / canvas->cell_size.height);

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
    struct yetty_yetty_ypaint_canvas_grid_line *line =
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
        struct yetty_yetty_ypaint_canvas_grid_cell *cell = &line->cells[x];
        for (uint32_t ri = 0; ri < cell->refs.count; ri++) {
          struct yetty_yetty_ypaint_canvas_prim_ref *ref = &cell->refs.data[ri];
          uint32_t bl = y + ref->lines_ahead;
          if (bl < canvas->lines.count && line_base_prim_idx) {
            ensure_grid_staging(canvas, canvas->grid_staging_count + 1);
            canvas->grid_staging[canvas->grid_staging_count++] =
                line_base_prim_idx[bl] + ref->prim_index;
            count++;
          } else if (ref->lines_ahead > 0) {
            // DEBUG: ref pointing outside valid lines!
            ydebug(
                "BUILD_GRID BUG: y=%u lines_ahead=%u bl=%u >= lines.count=%u",
                y, ref->lines_ahead, bl, canvas->lines.count);
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
  return YETTY_OK_VOID();
}

const uint32_t *
yetty_yetty_ypaint_canvas_grid_data(struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->grid_staging : NULL;
}

uint32_t yetty_yetty_ypaint_canvas_grid_word_count(
    struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->grid_staging_count : 0;
}

struct yetty_core_void_result yetty_yetty_ypaint_canvas_clear_staging(
    struct yetty_yetty_ypaint_canvas *canvas) {
  if (canvas) {
    canvas->grid_staging_count = 0;
    canvas->prim_staging_count = 0;
  }
  return YETTY_OK_VOID();
}

//=============================================================================
// Primitive staging
//=============================================================================

static struct yetty_core_void_result
ensure_prim_staging(struct yetty_yetty_ypaint_canvas *canvas,
                    uint32_t min_size) {
  if (min_size <= canvas->prim_staging_capacity)
    return YETTY_OK_VOID();

  uint32_t new_cap = canvas->prim_staging_capacity == 0
                         ? INITIAL_STAGING_CAPACITY
                         : canvas->prim_staging_capacity;
  while (new_cap < min_size)
    new_cap *= 2;

  canvas->prim_staging =
      realloc(canvas->prim_staging, new_cap * sizeof(uint32_t));
  canvas->prim_staging_capacity = new_cap;
  return YETTY_OK_VOID();
}

const uint32_t *yetty_yetty_ypaint_canvas_build_prim_staging(
    struct yetty_yetty_ypaint_canvas *canvas, uint32_t *word_count) {
  if (!canvas) {
    if (word_count)
      *word_count = 0;
    return NULL;
  }

  // Count primitives and total words (+1 per prim for rolling_row)
  uint32_t prim_count = 0;
  uint32_t total_words = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
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
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);

    for (uint32_t p = 0; p < line->prims.count; p++) {
      struct yetty_yetty_ypaint_canvas_prim_data *prim = &line->prims.data[p];
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

uint32_t yetty_yetty_ypaint_canvas_prim_gpu_size(
    struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return 0;

  uint32_t total_words = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    for (uint32_t p = 0; p < line->prims.count; p++)
      total_words += line->prims.data[p].word_count + 1; // +1 for rolling_row
  }
  return total_words * sizeof(float);
}

//=============================================================================
// State management
//=============================================================================

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_clear(struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");

  line_buffer_free(&canvas->lines);
  line_buffer_init(&canvas->lines);

  canvas->grid_staging_count = 0;
  canvas->prim_staging_count = 0;
  canvas->cursor_col = 0;
  canvas->cursor_row = 0;
  canvas->rolling_row_0 = 0;
  canvas->dirty = true;
  return YETTY_OK_VOID();
}

bool yetty_yetty_ypaint_canvas_empty(struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return true;

  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    if (line->prims.count > 0)
      return false;
  }
  return true;
}

uint32_t yetty_yetty_ypaint_canvas_primitive_count(
    struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return 0;

  uint32_t count = 0;
  for (uint32_t i = 0; i < canvas->lines.count; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    count += line->prims.count;
  }
  return count;
}
