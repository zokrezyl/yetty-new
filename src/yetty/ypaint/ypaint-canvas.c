// YPaint Canvas - Implementation
// Rolling offset approach for O(1) scrolling

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ypaint/flyweight.h>
#include <yetty/ypaint/core/ypaint-canvas.h>
#include <yetty/yfont/font.h>
#include <yetty/yfont/msdf-font.h>
#include <yetty/ysdf/types.gen.h>
#include <yetty/yconfig.h>
#include <yetty/yetty.h>
#include <yetty/ytrace.h>

/* Glyph primitive type (not in ysdf types.gen.h since not SDF) */
#define YETTY_YSDF_GLYPH 200

/* Glyph primitive: type, z_order, x, y, font_size, packed(glyph_idx|font_id), color */
#define YPAINT_GLYPH_WORDS 7

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

// Font resource attached to a grid line — pointer to the font object
struct yetty_yetty_ypaint_canvas_font_entry {
  struct yetty_font_font *font; // the font object (owns atlas + metadata)
  int32_t font_id;              // buffer-level font id
};

// Complex primitive reference (stored on last overlapping line)
struct yetty_yetty_ypaint_canvas_complex_prim {
  uint32_t *data;           // copied primitive data (owned)
  uint32_t word_count;      // size in words
  uint32_t rolling_row;     // rolling row at insertion
  void *cache;              // runtime cache (for gpu_resource_set)
};

// A single row/line in the grid
struct yetty_yetty_ypaint_canvas_grid_line {
  struct yetty_yetty_ypaint_canvas_prim_data_array
      prims; // All primitives (SDF + glyph) whose BASE is this line
  struct yetty_yetty_ypaint_canvas_grid_cell *cells;
  uint32_t cell_count;
  uint32_t cell_capacity;

  // Font resources owned by this line (moved down as needed)
  struct yetty_yetty_ypaint_canvas_font_entry *fonts;
  uint32_t font_count;
  uint32_t font_capacity;

  // Complex primitives whose BASE (last overlapping line) is this line
  struct yetty_yetty_ypaint_canvas_complex_prim *complex_prims;
  uint32_t complex_prim_count;
  uint32_t complex_prim_capacity;
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

  // Default font for text spans with font_id = -1
  struct yetty_font_font *default_font;

  // Shaders directory for creating fonts from buffers
  char shaders_dir[512];

  // Flyweight registry for primitive handlers
  struct yetty_ypaint_flyweight_registry *flyweight_registry;
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
  line->fonts = NULL;
  line->font_count = 0;
  line->font_capacity = 0;
  line->cells = NULL;
  line->cell_count = 0;
  line->cell_capacity = 0;
  line->complex_prims = NULL;
  line->complex_prim_count = 0;
  line->complex_prim_capacity = 0;
  if (initial_cells > 0) {
    line->cells = calloc(initial_cells,
                         sizeof(struct yetty_yetty_ypaint_canvas_grid_cell));
    if (!line->cells)
      return YETTY_ERR(yetty_core_void, "calloc failed for grid cells");
    line->cell_capacity = initial_cells;
  }
  return YETTY_OK_VOID();
}

static struct yetty_core_void_result
grid_line_free(struct yetty_yetty_ypaint_canvas_grid_line *line,
               const struct yetty_ypaint_flyweight_registry *reg) {
  if (!reg)
    return YETTY_ERR(yetty_core_void, "reg is NULL");

  prim_data_array_free(&line->prims);
  /* Destroy fonts owned by this line */
  for (uint32_t i = 0; i < line->font_count; i++) {
    if (line->fonts[i].font && line->fonts[i].font->ops)
      line->fonts[i].font->ops->destroy(line->fonts[i].font);
  }
  free(line->fonts);
  line->fonts = NULL;
  line->font_count = 0;
  line->font_capacity = 0;
  /* Destroy complex prims owned by this line */
  for (uint32_t i = 0; i < line->complex_prim_count; i++) {
    struct yetty_yetty_ypaint_canvas_complex_prim *cp = &line->complex_prims[i];
    if (cp->cache) {
      struct yetty_ypaint_prim_flyweight fw =
          yetty_ypaint_flyweight_registry_get(reg, cp->data);
      if (!fw.ops)
        return YETTY_ERR(yetty_core_void, "no handler for complex prim type");
      if (fw.ops->destroy)
        fw.ops->destroy(cp->cache);
    }
    free(cp->data);
  }
  free(line->complex_prims);
  line->complex_prims = NULL;
  line->complex_prim_count = 0;
  line->complex_prim_capacity = 0;
  for (uint32_t i = 0; i < line->cell_count; i++)
    prim_ref_array_free(&line->cells[i].refs);
  free(line->cells);
  line->cells = NULL;
  line->cell_count = 0;
  line->cell_capacity = 0;
  return YETTY_OK_VOID();
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

static struct yetty_core_void_result
line_buffer_free(struct yetty_yetty_ypaint_canvas_line_buffer *buf,
                 const struct yetty_ypaint_flyweight_registry *reg) {
  for (uint32_t i = 0; i < buf->count; i++) {
    struct yetty_core_void_result res = grid_line_free(&buf->lines[i], reg);
    if (YETTY_IS_ERR(res))
      return res;
  }
  free(buf->lines);
  buf->lines = NULL;
  buf->capacity = 0;
  buf->count = 0;
  return YETTY_OK_VOID();
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

static struct yetty_core_void_result
line_buffer_pop_front(struct yetty_yetty_ypaint_canvas_line_buffer *buf,
                      const struct yetty_ypaint_flyweight_registry *reg,
                      uint32_t count) {
  if (count == 0 || buf->count == 0)
    return YETTY_OK_VOID();
  if (count > buf->count)
    count = buf->count;

  // Free the top lines being removed
  for (uint32_t i = 0; i < count; i++) {
    struct yetty_core_void_result res = grid_line_free(&buf->lines[i], reg);
    if (YETTY_IS_ERR(res))
      return res;
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
  return YETTY_OK_VOID();
}

//=============================================================================
// Canvas implementation
//=============================================================================

struct yetty_yetty_ypaint_canvas *
yetty_yetty_ypaint_canvas_create(bool scrolling_mode,
                                  const struct yetty_context *context) {
  struct yetty_yetty_ypaint_canvas *canvas;

  if (!context)
    return NULL;

  canvas = calloc(1, sizeof(struct yetty_yetty_ypaint_canvas));
  if (!canvas)
    return NULL;

  canvas->scrolling_mode = scrolling_mode;
  canvas->dirty = true;
  canvas->rolling_row_0 = 0;

  line_buffer_init(&canvas->lines);

  /* Create flyweight registry with all handlers */
  struct yetty_ypaint_flyweight_registry_ptr_result fw_res =
      yetty_ypaint_flyweight_create();
  if (YETTY_IS_ERR(fw_res)) {
    yerror("ypaint_canvas: flyweight creation failed: %s", fw_res.error.msg);
    /* lines buffer is empty here, no registry needed for cleanup */
    free(canvas->lines.lines);
    free(canvas);
    return NULL;
  }
  canvas->flyweight_registry = fw_res.value;

  /* Create default MSDF font for text spans (font_id = -1) */
  struct yetty_config *config = context->app_context.config;
  const char *fonts_dir = config->ops->get_string(config, "paths/fonts", "");
  const char *shaders_dir = config->ops->get_string(config, "paths/shaders", "");
  const char *font_family = config->ops->font_family(config);
  if (!font_family || strcmp(font_family, "default") == 0)
    font_family = "DejaVuSansMNerdFontMono";
  char cdb_path[512];
  char shader_path[512];
  snprintf(cdb_path, sizeof(cdb_path), "%s/../msdf-fonts/%s-Regular.cdb",
           fonts_dir, font_family);
  snprintf(shader_path, sizeof(shader_path), "%s/msdf-font.wgsl", shaders_dir);
  strncpy(canvas->shaders_dir, shaders_dir, sizeof(canvas->shaders_dir) - 1);
  ydebug("ypaint_canvas: default font cdb_path='%s' shader='%s'", cdb_path, shader_path);
  struct yetty_font_font_result font_res = yetty_font_msdf_font_create(cdb_path, shader_path);
  if (YETTY_IS_OK(font_res)) {
    canvas->default_font = font_res.value;
    ydebug("ypaint_canvas: default font created");
  } else {
    yerror("ypaint_canvas: default font creation failed: %s", font_res.error.msg);
    yetty_ypaint_flyweight_registry_destroy(canvas->flyweight_registry);
    /* lines buffer is empty here, no registry needed for cleanup */
    free(canvas->lines.lines);
    free(canvas);
    return NULL;
  }

  return canvas;
}

struct yetty_core_void_result
yetty_yetty_ypaint_canvas_destroy(struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");

  if (canvas->default_font)
    canvas->default_font->ops->destroy(canvas->default_font);
  struct yetty_core_void_result res =
      line_buffer_free(&canvas->lines, canvas->flyweight_registry);
  if (YETTY_IS_ERR(res))
    return res;
  yetty_ypaint_flyweight_registry_destroy(canvas->flyweight_registry);
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
// Returns the grid_line (bottom row of AABB) for this primitive
static struct uint32_result
add_primitive_internal(struct yetty_yetty_ypaint_canvas *canvas,
                       const struct yetty_ypaint_core_primitive_iter *iter) {
  if (!canvas)
    return YETTY_ERR(uint32, "canvas is NULL");
  if (!iter || !iter->fw.data || !iter->fw.ops)
    return YETTY_ERR(uint32, "invalid iterator");
  if (canvas->cell_size.height <= 0.0f)
    return YETTY_ERR(uint32, "cell_height <= 0");
  if (canvas->cell_size.width <= 0.0f)
    return YETTY_ERR(uint32, "cell_width <= 0");

  if (!iter->fw.ops->aabb || !iter->fw.ops->size)
    return YETTY_ERR(uint32, "handler missing ops");
  struct rectangle_result aabb_res = iter->fw.ops->aabb(iter->fw.data);
  if (YETTY_IS_ERR(aabb_res))
    return YETTY_ERR(uint32, aabb_res.error.msg);
  struct rectangle aabb = aabb_res.value;

  struct yetty_core_size_result size_res = iter->fw.ops->size(iter->fw.data);
  if (YETTY_IS_ERR(size_res))
    return YETTY_ERR(uint32, size_res.error.msg);
  uint32_t word_count = size_res.value / sizeof(uint32_t);

  if (aabb.min.y > aabb.max.y) {
    yerror("BUG: inverted AABB! min.y=%.1f > max.y=%.1f", aabb.min.y,
           aabb.max.y);
    float tmp = aabb.min.y;
    aabb.min.y = aabb.max.y;
    aabb.max.y = tmp;
  }

  uint32_t primitive_max_in_rows =
      (uint32_t)floorf(aabb.max.y / canvas->cell_size.height);

  uint32_t primitive_grid_line = canvas->cursor_row + primitive_max_in_rows;
  uint32_t primitive_rolling_row = canvas->rolling_row_0 + canvas->cursor_row;

  canvas_ensure_lines(canvas, primitive_grid_line + 1);

  struct yetty_yetty_ypaint_canvas_grid_line *base_line =
      line_buffer_get(&canvas->lines, primitive_grid_line);
  if (!base_line)
    return YETTY_ERR(uint32, "line_buffer_get returned NULL");

  uint32_t prim_index = prim_data_array_push(
      &base_line->prims, primitive_rolling_row,
      (const float *)iter->fw.data, word_count);

  uint32_t prim_col_min =
      (uint32_t)(aabb.min.x / canvas->cell_size.width);
  uint32_t prim_col_max =
      (uint32_t)(aabb.max.x / canvas->cell_size.width);

  int32_t row_min_rel = (int32_t)floorf(aabb.min.y / canvas->cell_size.height);
  int32_t row_max_rel = (int32_t)floorf(aabb.max.y / canvas->cell_size.height);
  if (row_min_rel < 0)
    row_min_rel = 0;
  if (row_max_rel < 0)
    row_max_rel = 0;

  uint32_t prim_row_min = canvas->cursor_row + (uint32_t)row_min_rel;
  uint32_t prim_row_max = canvas->cursor_row + (uint32_t)row_max_rel;

  if (prim_row_min > prim_row_max)
    return YETTY_ERR(uint32, "AABB row min > max after clamp");
  if (prim_col_min > prim_col_max)
    return YETTY_ERR(uint32, "AABB col min > max");

  if (canvas->grid_size.cols == 0)
    return YETTY_ERR(uint32, "grid_size.cols is 0");
  if (prim_col_max >= canvas->grid_size.cols)
    prim_col_max = canvas->grid_size.cols - 1;

  for (uint32_t row = prim_row_min; row <= prim_row_max; row++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, row);
    grid_line_ensure_cells(line, prim_col_max + 1);

    uint16_t lines_ahead = (uint16_t)(primitive_grid_line - row);

    for (uint32_t col = prim_col_min; col <= prim_col_max; col++) {
      struct yetty_yetty_ypaint_canvas_prim_ref ref = {lines_ahead,
                                                       (uint16_t)prim_index};
      prim_ref_array_push(&line->cells[col].refs, ref);
    }
  }

  ydebug("add_primitive_internal: aabb_y=[%.1f,%.1f] cell_height=%.1f "
         "cursor_row=%u",
         aabb.min.y, aabb.max.y, canvas->cell_size.height, canvas->cursor_row);
  ydebug(
      "add_primitive_internal: prim_min_row=%u prim_max_row=%u lines.count=%u",
      prim_row_min, prim_row_max, canvas->lines.count);

  canvas->dirty = true;
  return YETTY_OK(uint32, primitive_grid_line);
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

  // Check if buffer has SDF primitives (text spans handled separately in PASS 4)
  struct yetty_ypaint_core_primitive_iter_result iter_res =
      yetty_ypaint_core_buffer_prim_first(buffer, canvas->flyweight_registry);
  bool has_sdf_primitives = YETTY_IS_OK(iter_res);

  // Save original cursor - primitives coords are relative to THIS position
  uint16_t original_cursor_row = canvas->cursor_row;
  uint32_t original_rolling_row_0 = canvas->rolling_row_0;
  (void)original_rolling_row_0;

  ydebug("add_buffer: START cursor_row=%u grid_rows=%u rolling_row_0=%u has_sdf=%d",
         canvas->cursor_row, canvas->grid_size.rows, canvas->rolling_row_0,
         has_sdf_primitives);

  uint16_t lines_scrolled = 0;
  uint32_t max_row_seen = 0;

  // PASS 1: Compute max_row needed for ALL content (SDF + text spans)
  uint32_t max_row_needed = 0;
  float cursor_y_offset = original_cursor_row * canvas->cell_size.height;

  // PASS 1a: SDF primitives
  if (has_sdf_primitives) {
    struct yetty_ypaint_core_primitive_iter iter = iter_res.value;

    while (1) {
      if (!iter.fw.ops->aabb)
        break;
      struct rectangle_result aabb_res = iter.fw.ops->aabb(iter.fw.data);
      if (YETTY_IS_ERR(aabb_res))
        break;
      struct rectangle aabb = aabb_res.value;

      float abs_max_y = aabb.max.y + cursor_y_offset;
      uint32_t prim_max_row =
          (canvas->cell_size.height > 0)
              ? (uint32_t)floorf(abs_max_y / canvas->cell_size.height)
              : 0;

      if (prim_max_row > max_row_needed)
        max_row_needed = prim_max_row;

      iter_res = yetty_ypaint_core_buffer_prim_next(buffer, canvas->flyweight_registry, &iter);
      if (YETTY_IS_ERR(iter_res))
        break;
      iter = iter_res.value;
    }
  }

  // PASS 1b: Text spans - estimate max row from position + font size
  uint32_t span_count = yetty_ypaint_core_buffer_text_span_count(buffer);
  for (uint32_t si = 0; si < span_count; si++) {
    const struct yetty_text_span *ts =
        yetty_ypaint_core_buffer_get_text_span(buffer, si);
    if (!ts || !ts->named_buf.buf.data || ts->named_buf.buf.size == 0)
      continue;

    // Estimate: baseline at ts->y, glyph extends ~font_size below baseline
    // (conservative estimate - actual depends on font metrics)
    float estimated_max_y = ts->y + ts->font_size;
    float abs_max_y = estimated_max_y + cursor_y_offset;
    uint32_t text_max_row =
        (canvas->cell_size.height > 0)
            ? (uint32_t)floorf(abs_max_y / canvas->cell_size.height)
            : 0;

    if (text_max_row > max_row_needed)
      max_row_needed = text_max_row;
  }

  ydebug("add_buffer: PASS1 max_row_needed=%u (cursor at row %u)",
         max_row_needed, original_cursor_row);

  // SCROLL if content extends beyond visible area
  uint32_t target_cursor_row = max_row_needed + 1;
  if (target_cursor_row >= canvas->grid_size.rows) {
    lines_scrolled = (uint16_t)(target_cursor_row - canvas->grid_size.rows + 1);

    ydebug("add_buffer: SCROLL NEEDED target=%u >= grid_rows=%u, scroll %u",
           target_cursor_row, canvas->grid_size.rows, lines_scrolled);

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

    yetty_yetty_ypaint_canvas_scroll_lines(canvas, lines_scrolled);

    ydebug("add_buffer: after scroll cursor_row=%u rolling_row_0=%u",
           canvas->cursor_row, canvas->rolling_row_0);
  }

  // PASS 2: Add SDF primitives with adjusted cursor
  uint16_t adjusted_cursor = (original_cursor_row >= lines_scrolled)
                                 ? (original_cursor_row - lines_scrolled)
                                 : 0;
  canvas->cursor_row = adjusted_cursor;

  if (has_sdf_primitives) {
    ydebug("add_buffer: PASS2 using adjusted cursor_row=%u (original=%u - "
           "scrolled=%u)",
           adjusted_cursor, original_cursor_row, lines_scrolled);

    iter_res = yetty_ypaint_core_buffer_prim_first(buffer, canvas->flyweight_registry);
    struct yetty_ypaint_core_primitive_iter iter = iter_res.value;

    while (1) {
      struct uint32_result prim_res = add_primitive_internal(canvas, &iter);
      if (YETTY_IS_ERR(prim_res)) {
        yerror("add_buffer: add_primitive_internal failed: %s",
               prim_res.error.msg);
        return YETTY_ERR(yetty_core_void, prim_res.error.msg);
      }
      uint32_t prim_max_row = prim_res.value;

      ydebug("add_buffer: PASS2 added prim type=%u max_row=%u", iter.fw.data[0],
             prim_max_row);

      if (prim_max_row > max_row_seen)
        max_row_seen = prim_max_row;

      iter_res = yetty_ypaint_core_buffer_prim_next(buffer, canvas->flyweight_registry, &iter);
      if (YETTY_IS_ERR(iter_res))
        break;
      iter = iter_res.value;
    }
  } // end if (has_sdf_primitives)

  // PASS 3: Process font blobs → create MSDF font objects
  uint32_t font_count = yetty_ypaint_core_buffer_font_count(buffer);
  struct yetty_font_font **fonts = NULL;
  if (font_count > 0) {
    fonts = calloc(font_count, sizeof(struct yetty_font_font *));
    for (uint32_t i = 0; i < font_count; i++) {
      const struct yetty_font_blob *fb =
          yetty_ypaint_core_buffer_get_font(buffer, i);
      if (!fb || !fb->named_buf.buf.data) continue;
      // For now, fonts come as pre-built CDB paths in the name field
      // TODO: support inline TTF → MSDF generation
      char font_shader_path[512];
      snprintf(font_shader_path, sizeof(font_shader_path), "%s/msdf-font.wgsl",
               canvas->shaders_dir);
      struct yetty_font_font_result fr =
          yetty_font_msdf_font_create(fb->named_buf.name, font_shader_path);
      if (YETTY_IS_ERR(fr)) {
        yerror("add_buffer: font creation failed: %s", fr.error.msg);
        free(fonts);
        return YETTY_ERR(yetty_core_void, fr.error.msg);
      }
      fonts[i] = fr.value;
    }
  }

  // PASS 4: Process text spans → decompose into glyph primitives
  // (span_count already computed in PASS 1b)
  ydebug("add_buffer: PASS4 span_count=%u default_font=%p",
         span_count, (void *)canvas->default_font);
  for (uint32_t si = 0; si < span_count; si++) {
    const struct yetty_text_span *ts =
        yetty_ypaint_core_buffer_get_text_span(buffer, si);
    if (!ts || !ts->named_buf.buf.data || ts->named_buf.buf.size == 0)
      continue;

    ydebug("add_buffer: PASS4 span[%u] font_id=%d x=%.1f y=%.1f",
           si, ts->font_id, ts->x, ts->y);

    struct yetty_font_font *font = NULL;
    if (ts->font_id >= 0 && (uint32_t)ts->font_id < font_count)
      font = fonts[ts->font_id];
    if (!font && ts->font_id == -1)
      font = canvas->default_font;
    if (!font) {
      yerror("add_buffer: span[%u] font_id=%d not found and no default", si, ts->font_id);
      if (fonts) free(fonts);
      return YETTY_ERR(yetty_core_void, "text span font not found");
    }

    float base_size = font->ops->get_base_size(font);
    float scale = (base_size > 0) ? ts->font_size / base_size : 1.0f;
    float cursor_x = ts->x;
    uint32_t glyph_max_row = 0;

    const uint8_t *ptr = ts->named_buf.buf.data;
    const uint8_t *end = ptr + ts->named_buf.buf.size;

    while (ptr < end) {
      /* UTF-8 decode */
      uint32_t cp = 0;
      if ((*ptr & 0x80) == 0) {
        cp = *ptr++;
      } else if ((*ptr & 0xE0) == 0xC0) {
        cp = (*ptr++ & 0x1F) << 6;
        if (ptr < end) cp |= (*ptr++ & 0x3F);
      } else if ((*ptr & 0xF0) == 0xE0) {
        cp = (*ptr++ & 0x0F) << 12;
        if (ptr < end) cp |= (*ptr++ & 0x3F) << 6;
        if (ptr < end) cp |= (*ptr++ & 0x3F);
      } else if ((*ptr & 0xF8) == 0xF0) {
        cp = (*ptr++ & 0x07) << 18;
        if (ptr < end) cp |= (*ptr++ & 0x3F) << 12;
        if (ptr < end) cp |= (*ptr++ & 0x3F) << 6;
        if (ptr < end) cp |= (*ptr++ & 0x3F);
      } else {
        ptr++;
        continue;
      }

      struct uint32_result gi_res = font->ops->get_glyph_index(font, cp);
      if (YETTY_IS_ERR(gi_res)) {
        ydebug("add_buffer: glyph_index FAILED cp=0x%x: %s", cp, gi_res.error.msg);
        cursor_x += ts->font_size * 0.5f;
        continue;
      }
      uint32_t glyph_index = gi_res.value;

      /* Read glyph metadata from font's metadata buffer */
      struct yetty_render_gpu_resource_set_result rs_res =
          font->ops->get_gpu_resource_set(font);
      if (YETTY_IS_ERR(rs_res)) {
        ydebug("add_buffer: get_gpu_resource_set FAILED: %s", rs_res.error.msg);
        continue;
      }
      const struct yetty_render_gpu_resource_set *rs = rs_res.value;
      if (rs->buffer_count == 0 || !rs->buffers[0].data) {
        ydebug("add_buffer: no buffer data buf_count=%zu data=%p", rs->buffer_count, (void*)rs->buffers[0].data);
        continue;
      }

      /* Metadata: 6 floats per glyph [size_x, size_y, bearing_x, bearing_y, advance, _pad] */
      const float *meta = (const float *)rs->buffers[0].data;
      uint32_t meta_count = (uint32_t)(rs->buffers[0].size / (6 * sizeof(float)));
      ydebug("add_buffer: cp=0x%x glyph_index=%u meta_count=%u buf_size=%zu", cp, glyph_index, meta_count, rs->buffers[0].size);
      if (glyph_index >= meta_count) {
        ydebug("add_buffer: glyph_index %u >= meta_count %u, skipping", glyph_index, meta_count);
        cursor_x += ts->font_size * 0.5f;
        continue;
      }

      const float *gm = meta + glyph_index * 6;
      float size_x = gm[0], size_y = gm[1];
      float bearing_x = gm[2], bearing_y = gm[3];
      float advance = gm[4];

      /* Skip empty glyphs (space, etc.) - just advance cursor */
      if (size_x <= 0.0f || size_y <= 0.0f) {
        cursor_x += advance * scale;
        continue;
      }

      float gx = cursor_x + bearing_x * scale;
      float gy = ts->y - bearing_y * scale;
      float gw = size_x * scale;
      float gh = size_y * scale;

      /* Pack glyph primitive (7 words): type, z_order, x, y, font_size, packed, color */
      static uint32_t glyph_z_order = 0;
      float glyph_data[YPAINT_GLYPH_WORDS];
      uint32_t tmp;
      tmp = YETTY_YSDF_GLYPH;
      memcpy(&glyph_data[0], &tmp, sizeof(float));
      tmp = glyph_z_order++;
      memcpy(&glyph_data[1], &tmp, sizeof(float));
      glyph_data[2] = gx;
      glyph_data[3] = gy;
      glyph_data[4] = ts->font_size;  /* target render size */
      /* Pack glyph_index (16 bits) | font_id (16 bits) */
      uint32_t packed_gf = (glyph_index & 0xFFFF) |
                           (((uint32_t)(ts->font_id + 1) & 0xFFFF) << 16);
      memcpy(&glyph_data[5], &packed_gf, sizeof(float));
      /* Color */
      uint32_t color = ts->color.r | (ts->color.g << 8) |
                       (ts->color.b << 16) | (ts->color.a << 24);
      memcpy(&glyph_data[6], &color, sizeof(float));

      /* Compute glyph AABB in absolute coords */
      float abs_y = gy + canvas->cursor_row * canvas->cell_size.height;
      float abs_y_max = abs_y + gh;
      uint32_t glyph_row_max =
          (uint32_t)(abs_y_max / canvas->cell_size.height);

      canvas_ensure_lines(canvas, glyph_row_max + 1);

      uint32_t rolling_row = canvas->rolling_row_0 + canvas->cursor_row;

      /* Store glyph as primitive at its max row */
      struct yetty_yetty_ypaint_canvas_grid_line *base_line =
          line_buffer_get(&canvas->lines, glyph_row_max);
      if (!base_line) goto next_glyph;

      uint32_t prim_idx = prim_data_array_push(&base_line->prims,
                                               rolling_row, glyph_data,
                                               YPAINT_GLYPH_WORDS);

      /* Register in grid cells */
      uint32_t col_min = (canvas->cell_size.width > 0)
          ? (uint32_t)(gx / canvas->cell_size.width) : 0;
      uint32_t col_max = (canvas->cell_size.width > 0)
          ? (uint32_t)((gx + gw) / canvas->cell_size.width) : 0;
      uint32_t row_min = (uint32_t)(abs_y / canvas->cell_size.height);

      if (col_max >= canvas->grid_size.cols && canvas->grid_size.cols > 0)
        col_max = canvas->grid_size.cols - 1;

      for (uint32_t row = row_min; row <= glyph_row_max; row++) {
        struct yetty_yetty_ypaint_canvas_grid_line *line =
            line_buffer_get(&canvas->lines, row);
        grid_line_ensure_cells(line, col_max + 1);
        uint16_t lines_ahead = (uint16_t)(glyph_row_max - row);
        for (uint32_t col = col_min; col <= col_max; col++) {
          struct yetty_yetty_ypaint_canvas_prim_ref ref = {
              lines_ahead, (uint16_t)prim_idx};
          prim_ref_array_push(&line->cells[col].refs, ref);
        }
      }

      if (glyph_row_max > glyph_max_row)
        glyph_max_row = glyph_row_max;
      if (glyph_row_max > max_row_seen)
        max_row_seen = glyph_row_max;

next_glyph:
      cursor_x += advance * scale;
    }

    /* Attach font to the lowest row this span's glyphs reach.
     * Skip default font - it's owned by canvas, not by lines. */
    if (font && font != canvas->default_font && glyph_max_row > 0) {
      struct yetty_yetty_ypaint_canvas_grid_line *target_line =
          line_buffer_get(&canvas->lines, glyph_max_row);
      if (target_line) {
        /* Check if font already attached to a higher line — move it down */
        bool found = false;
        for (uint32_t li = 0; li < canvas->lines.count && !found; li++) {
          struct yetty_yetty_ypaint_canvas_grid_line *l =
              &canvas->lines.lines[li];
          for (uint32_t fi = 0; fi < l->font_count; fi++) {
            if (l->fonts[fi].font == font) {
              /* Move from old line to target line */
              if (li != glyph_max_row) {
                /* Add to target */
                if (target_line->font_count >= target_line->font_capacity) {
                  uint32_t new_cap = target_line->font_capacity == 0
                      ? 4 : target_line->font_capacity * 2;
                  target_line->fonts = realloc(target_line->fonts,
                      new_cap * sizeof(struct yetty_yetty_ypaint_canvas_font_entry));
                  target_line->font_capacity = new_cap;
                }
                target_line->fonts[target_line->font_count++] = l->fonts[fi];
                /* Remove from old line */
                l->fonts[fi] = l->fonts[--l->font_count];
              }
              found = true;
              break;
            }
          }
        }
        if (!found) {
          /* First time — attach to target line */
          if (target_line->font_count >= target_line->font_capacity) {
            uint32_t new_cap = target_line->font_capacity == 0
                ? 4 : target_line->font_capacity * 2;
            target_line->fonts = realloc(target_line->fonts,
                new_cap * sizeof(struct yetty_yetty_ypaint_canvas_font_entry));
            target_line->font_capacity = new_cap;
          }
          struct yetty_yetty_ypaint_canvas_font_entry entry = {0};
          entry.font = font;
          entry.font_id = ts->font_id;
          target_line->fonts[target_line->font_count++] = entry;
          /* Null out from fonts array so it's not double-freed */
          if (ts->font_id >= 0 && (uint32_t)ts->font_id < font_count)
            fonts[ts->font_id] = NULL;
        }
      }
    }
  }

  /* Free any fonts not attached to grid lines */
  if (fonts) {
    for (uint32_t i = 0; i < font_count; i++) {
      if (fonts[i] && fonts[i]->ops)
        fonts[i]->ops->destroy(fonts[i]);
    }
    free(fonts);
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
  struct yetty_core_void_result pop_res =
      line_buffer_pop_front(&canvas->lines, canvas->flyweight_registry, num_lines);
  if (YETTY_IS_ERR(pop_res))
    return pop_res;

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

  if (grid_w == 0 || grid_h == 0) {
    canvas->grid_staging_count = 0;
    canvas->dirty = false;
    free(line_base_prim_idx);
    return YETTY_OK_VOID();
  }

  uint32_t num_cells = grid_w * grid_h;

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

      // Add prim entries (includes both SDF and glyph primitives)
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
          }
        }
      }

      canvas->grid_staging[count_pos] = count;
    }
  }

  free(line_base_prim_idx);
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

  struct yetty_core_void_result res =
      line_buffer_free(&canvas->lines, canvas->flyweight_registry);
  if (YETTY_IS_ERR(res))
    return res;
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

struct yetty_font_font *yetty_yetty_ypaint_canvas_get_default_font(
    struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->default_font : NULL;
}

//=============================================================================
// Complex primitive access (for atlas rendering)
//=============================================================================

uint32_t yetty_yetty_ypaint_canvas_complex_prim_count(
    struct yetty_yetty_ypaint_canvas *canvas) {
  if (!canvas)
    return 0;

  uint32_t count = 0;
  uint32_t visible_lines = canvas->grid_size.rows;
  if (visible_lines > canvas->lines.count)
    visible_lines = canvas->lines.count;

  for (uint32_t i = 0; i < visible_lines; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    count += line->complex_prim_count;
  }
  return count;
}

struct yetty_ypaint_canvas_complex_prim_ref
yetty_yetty_ypaint_canvas_get_complex_prim(
    struct yetty_yetty_ypaint_canvas *canvas, uint32_t index) {
  struct yetty_ypaint_canvas_complex_prim_ref ref = {NULL, NULL};
  if (!canvas)
    return ref;

  uint32_t visible_lines = canvas->grid_size.rows;
  if (visible_lines > canvas->lines.count)
    visible_lines = canvas->lines.count;

  uint32_t current = 0;
  for (uint32_t i = 0; i < visible_lines; i++) {
    struct yetty_yetty_ypaint_canvas_grid_line *line =
        line_buffer_get(&canvas->lines, i);
    if (index < current + line->complex_prim_count) {
      uint32_t local_idx = index - current;
      struct yetty_yetty_ypaint_canvas_complex_prim *cp =
          &line->complex_prims[local_idx];
      ref.data = cp->data;
      ref.cache_ptr = &cp->cache;
      return ref;
    }
    current += line->complex_prim_count;
  }
  return ref;
}

const struct yetty_ypaint_flyweight_registry *
yetty_yetty_ypaint_canvas_get_flyweight_registry(
    struct yetty_yetty_ypaint_canvas *canvas) {
  return canvas ? canvas->flyweight_registry : NULL;
}
