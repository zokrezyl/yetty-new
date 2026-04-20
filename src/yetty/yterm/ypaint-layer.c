#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/util.h>
#include <yetty/yfont/font.h>
#include <yetty/ypaint-core/buffer.h>
#include <yetty/ypaint/core/ypaint-canvas.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/ypaint-yaml/ypaint-yaml.h>
#include <yetty/yterm/osc-args.h>
#include <yetty/yterm/ypaint-layer.h>
#include <yetty/yconfig.h>
#include <yetty/yetty.h>
#include <yetty/ytrace.h>


/* Uniform positions */
#define U_GRID_SIZE 0
#define U_CELL_SIZE 1
#define U_ROLLING_ROW_0 2
#define U_PRIM_COUNT 3
#define U_COUNT 4

/* Setters */
static inline void set_grid_size(struct yetty_render_gpu_resource_set *rs,
                                 float cols, float rows) {
  rs->uniforms[U_GRID_SIZE].vec2[0] = cols;
  rs->uniforms[U_GRID_SIZE].vec2[1] = rows;
}
static inline void set_cell_size(struct yetty_render_gpu_resource_set *rs,
                                 float w, float h) {
  rs->uniforms[U_CELL_SIZE].vec2[0] = w;
  rs->uniforms[U_CELL_SIZE].vec2[1] = h;
}
static inline void set_rolling_row_0(struct yetty_render_gpu_resource_set *rs,
                                     uint32_t row_origin) {
  rs->uniforms[U_ROLLING_ROW_0].u32 = row_origin;
}
static inline void set_prim_count(struct yetty_render_gpu_resource_set *rs,
                                  uint32_t count) {
  rs->uniforms[U_PRIM_COUNT].u32 = count;
}

/* Init uniforms */
static void init_uniforms(struct yetty_render_gpu_resource_set *rs) {
  rs->uniform_count = U_COUNT;

  rs->uniforms[U_GRID_SIZE] = (struct yetty_render_uniform){
      "ypaint_grid_size", YETTY_RENDER_UNIFORM_VEC2};
  rs->uniforms[U_CELL_SIZE] = (struct yetty_render_uniform){
      "ypaint_cell_size", YETTY_RENDER_UNIFORM_VEC2};
  rs->uniforms[U_ROLLING_ROW_0] = (struct yetty_render_uniform){
      "ypaint_rolling_row_0", YETTY_RENDER_UNIFORM_U32};
  rs->uniforms[U_PRIM_COUNT] = (struct yetty_render_uniform){
      "ypaint_prim_count", YETTY_RENDER_UNIFORM_U32};

  set_rolling_row_0(rs, 0);
  set_prim_count(rs, 0);
}

/* YPaint layer - embeds base as first member */
struct yetty_term_ypaint_layer {
  struct yetty_term_terminal_layer base;
  struct yetty_yetty_ypaint_canvas *canvas;
  int scrolling_mode;
  struct yetty_render_gpu_resource_set rs;
  struct yetty_core_buffer shader_code;

  /* Staging buffers - point to canvas data */
  uint8_t *grid_staging;
  size_t grid_staging_size;
  uint8_t *prim_staging;
  size_t prim_staging_size;
};

/* Forward declarations */
static void ypaint_layer_destroy(struct yetty_term_terminal_layer *self);
static struct yetty_core_void_result
ypaint_layer_write(struct yetty_term_terminal_layer *self,
                   const char *data, size_t len);
static struct yetty_core_void_result
ypaint_layer_resize_grid(struct yetty_term_terminal_layer *self,
                         struct grid_size grid_size);
static struct yetty_render_gpu_resource_set_result
ypaint_layer_get_gpu_resource_set(const struct yetty_term_terminal_layer *self);
static int ypaint_layer_on_key(struct yetty_term_terminal_layer *self, int key,
                               int mods);
static int ypaint_layer_on_char(struct yetty_term_terminal_layer *self,
                                uint32_t codepoint, int mods);
static int ypaint_layer_is_empty(const struct yetty_term_terminal_layer *self);
static struct yetty_core_void_result ypaint_layer_scroll(
    struct yetty_term_terminal_layer *self, int lines);
static void ypaint_layer_set_cursor(struct yetty_term_terminal_layer *self,
                                    int col, int row);

/* Canvas scroll callback - propagate to other layers */
static struct yetty_core_void_result
on_canvas_scroll(struct yetty_core_void_result *user_data, uint16_t num_lines) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)user_data;
  ydebug("on_canvas_scroll ENTER: num_lines=%u in_external=%d", num_lines,
         layer->base.in_external_scroll);

  /* If in_external_scroll is set, this scroll was triggered by another layer
   * and we should NOT propagate back to avoid double-scroll loop */
  if (layer->base.in_external_scroll) {
    ydebug("on_canvas_scroll: skipping (in_external_scroll)");
    return YETTY_OK_VOID();
  }

  if (!layer->base.scroll_fn) {
    yerror("on_canvas_scroll: scroll_fn is NULL");
    return YETTY_ERR(yetty_core_void, "scroll_fn is NULL");
  }
  struct yetty_core_void_result res = layer->base.scroll_fn(
      &layer->base, (int)num_lines, layer->base.scroll_userdata);
  if (YETTY_IS_ERR(res)) {
    yerror("on_canvas_scroll: scroll_fn failed: %s", res.error.msg);
    return res;
  }
  ydebug("on_canvas_scroll EXIT: num_lines=%u", num_lines);
  return YETTY_OK_VOID();
}

/* Canvas cursor callback - propagate to other layers */
static struct yetty_core_void_result
on_canvas_cursor_set(struct yetty_core_void_result *user_data,
                     uint16_t new_row) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)user_data;
  ydebug("on_canvas_cursor_set ENTER: new_row=%u", new_row);
  if (!layer->base.cursor_fn) {
    yerror("on_canvas_cursor_set: cursor_fn is NULL");
    return YETTY_ERR(yetty_core_void, "cursor_fn is NULL");
  }
  layer->base.cursor_fn(&layer->base,
                        (struct grid_cursor_pos){.cols = 0, .rows = new_row},
                        layer->base.cursor_userdata);
  ydebug("on_canvas_cursor_set EXIT: new_row=%u", new_row);
  return YETTY_OK_VOID();
}

/* Ops */
static const struct yetty_term_terminal_layer_ops ypaint_layer_ops = {
    .destroy = ypaint_layer_destroy,
    .write = ypaint_layer_write,
    .resize_grid = ypaint_layer_resize_grid,
    .get_gpu_resource_set = ypaint_layer_get_gpu_resource_set,
    .is_empty = ypaint_layer_is_empty,
    .on_key = ypaint_layer_on_key,
    .on_char = ypaint_layer_on_char,
    .scroll = ypaint_layer_scroll,
    .set_cursor = ypaint_layer_set_cursor,
};

/* Create */
struct yetty_term_terminal_layer_result yetty_term_ypaint_layer_create(
    uint32_t cols, uint32_t rows, float cell_width, float cell_height,
    int scrolling_mode, const struct yetty_context *context,
    yetty_term_request_render_fn request_render_fn,
    void *request_render_userdata, yetty_term_scroll_fn scroll_fn,
    void *scroll_userdata, yetty_term_cursor_fn cursor_fn,
    void *cursor_userdata) {
  struct yetty_term_ypaint_layer *layer;

  /* Load ypaint-layer shader from file */
  struct yetty_config *config = context->app_context.config;
  const char *shaders_dir = config->ops->get_string(config, "paths/shaders", "");
  char shader_path[512];
  snprintf(shader_path, sizeof(shader_path), "%s/ypaint-layer.wgsl", shaders_dir);
  struct yetty_core_buffer_result shader_res = yetty_core_read_file(shader_path);
  if (YETTY_IS_ERR(shader_res))
    return YETTY_ERR(yetty_term_terminal_layer, shader_res.error.msg);

  layer = calloc(1, sizeof(struct yetty_term_ypaint_layer));
  if (!layer) {
    free(shader_res.value.data);
    return YETTY_ERR(yetty_term_terminal_layer,
                     "failed to allocate ypaint layer");
  }
  layer->shader_code = shader_res.value;

  layer->base.ops = &ypaint_layer_ops;
  layer->base.grid_size.cols = cols;
  layer->base.grid_size.rows = rows;
  layer->base.cell_size.width = cell_width;
  layer->base.cell_size.height = cell_height;
  layer->base.dirty = 0;
  layer->base.pty_write_fn = NULL; /* ypaint layer doesn't write to PTY */
  layer->base.pty_write_userdata = NULL;
  layer->base.request_render_fn = request_render_fn;
  layer->base.request_render_userdata = request_render_userdata;
  layer->base.scroll_fn = scroll_fn;
  layer->base.scroll_userdata = scroll_userdata;
  layer->base.cursor_fn = cursor_fn;
  layer->base.cursor_userdata = cursor_userdata;

  layer->scrolling_mode = scrolling_mode;

  /* Create canvas (passes context for default font creation) */
  if (!context) {
    free(layer);
    return YETTY_ERR(yetty_term_terminal_layer, "context is NULL");
  }
  layer->canvas =
      yetty_yetty_ypaint_canvas_create(scrolling_mode ? true : false, context);
  if (!layer->canvas) {
    free(layer);
    return YETTY_ERR(yetty_term_terminal_layer,
                     "failed to create ypaint canvas");
  }

  /* Configure canvas grid/cell dimensions */
  yetty_yetty_ypaint_canvas_set_cell_size(
      layer->canvas,
      (struct pixel_size){.width = cell_width, .height = cell_height});
  yetty_yetty_ypaint_canvas_set_grid_size(
      layer->canvas, (struct grid_size){.cols = cols, .rows = rows});

  /* Register scroll/cursor callbacks for propagation to other layers */
  yetty_yetty_ypaint_canvas_set_scroll_callback(
      layer->canvas, on_canvas_scroll, (struct yetty_core_void_result *)layer);
  yetty_yetty_ypaint_canvas_set_cursor_callback(
      layer->canvas, on_canvas_cursor_set,
      (struct yetty_core_void_result *)layer);

  /* Resource set */
  strncpy(layer->rs.namespace,
          scrolling_mode ? "ypaint_scroll" : "ypaint_overlay",
          YETTY_RENDER_NAME_MAX - 1);

  /* Buffer 0: grid staging (cell-to-primitive lookup) */
  layer->rs.buffer_count = 2;
  layer->rs.children_count = 1;  /* Reserve slot for font resource set */
  strncpy(layer->rs.buffers[0].name, "grid", YETTY_RENDER_NAME_MAX - 1);
  strncpy(layer->rs.buffers[0].wgsl_type, "array<u32>",
          YETTY_RENDER_WGSL_TYPE_MAX - 1);
  layer->rs.buffers[0].readonly = 1;

  /* Buffer 1: primitive staging (serialized primitives) */
  strncpy(layer->rs.buffers[1].name, "prims", YETTY_RENDER_NAME_MAX - 1);
  strncpy(layer->rs.buffers[1].wgsl_type, "array<u32>",
          YETTY_RENDER_WGSL_TYPE_MAX - 1);
  layer->rs.buffers[1].readonly = 1;

  /* Initialize uniforms - actual values set in get_gpu_resource_set from canvas
   */
  init_uniforms(&layer->rs);

  /* Set initial pixel size for render target */
  layer->rs.pixel_size.width = (float)cols * cell_width;
  layer->rs.pixel_size.height = (float)rows * cell_height;

  yetty_render_shader_code_set(&layer->rs.shader,
                               (const char *)layer->shader_code.data,
                               layer->shader_code.size);

  ydebug("ypaint_layer_create: %s mode, %ux%u grid, %.1fx%.1f cells",
         scrolling_mode ? "scrolling" : "overlay", cols, rows, cell_width,
         cell_height);

  return YETTY_OK(yetty_term_terminal_layer, &layer->base);
}

/* Destroy */
static void ypaint_layer_destroy(struct yetty_term_terminal_layer *self) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)self;

  if (layer->canvas)
    yetty_yetty_ypaint_canvas_destroy(layer->canvas);

  free(layer->shader_code.data);
  free(layer);
}

/* Write - receives ypaint data in format "args;payload" (base64 encoded) */
static struct yetty_core_void_result
ypaint_layer_write(struct yetty_term_terminal_layer *self,
                   const char *data, size_t len) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)self;
  struct yetty_term_osc_args args;

  if (!data || len == 0)
    return YETTY_ERR(yetty_core_void, "no data");

  /* Parse OSC args */
  if (yetty_term_osc_args_parse(&args, data, len) < 0)
    return YETTY_ERR(yetty_core_void, "failed to parse args");

  /* Handle --clear */
  if (yetty_term_osc_args_has(&args, "clear")) {
    ydebug("ypaint_layer_write: clearing canvas");
    yetty_yetty_ypaint_canvas_clear(layer->canvas);
    yetty_term_osc_args_free(&args);
    layer->base.dirty = 1;
    if (layer->base.request_render_fn)
      layer->base.request_render_fn(layer->base.request_render_userdata);
    return YETTY_OK_VOID();
  }

  /* Check for payload */
  if (!args.payload || args.payload_len == 0) {
    ydebug("ypaint_layer_write: no payload");
    yetty_term_osc_args_free(&args);
    return YETTY_OK_VOID();
  }

  /* Handle format */
  if (yetty_term_osc_args_has(&args, "yaml")) {
    /* YAML format: decode base64, then parse YAML */
    size_t decoded_cap = args.payload_len;
    char *decoded = malloc(decoded_cap + 1);
    if (!decoded) {
      yetty_term_osc_args_free(&args);
      return YETTY_ERR(yetty_core_void, "malloc failed");
    }
    size_t decoded_len = yetty_core_base64_decode(args.payload, args.payload_len,
                                                  decoded, decoded_cap);
    decoded[decoded_len] = '\0';

    ydebug("ypaint_layer_write: yaml payload_len=%zu decoded_len=%zu",
           args.payload_len, decoded_len);

    struct yetty_ypaint_core_buffer_result res =
        yetty_ypaint_yaml_parse(decoded, decoded_len);
    free(decoded);

    if (YETTY_IS_ERR(res)) {
      yetty_term_osc_args_free(&args);
      return YETTY_ERR(yetty_core_void, res.error.msg);
    }

    struct yetty_core_void_result add_res =
        yetty_ypaint_canvas_add_buffer(layer->canvas, res.value);
    yetty_ypaint_core_buffer_destroy(res.value);
    if (YETTY_IS_ERR(add_res)) {
      yetty_term_osc_args_free(&args);
      return add_res;
    }
  } else if (yetty_term_osc_args_has(&args, "bin")) {
    /* Binary format: pass base64 payload directly to create_from_base64 */
    struct yetty_core_buffer_result payload = yetty_term_osc_args_get_payload_buffer(&args);
    if (YETTY_IS_ERR(payload)) {
      yetty_term_osc_args_free(&args);
      return YETTY_ERR(yetty_core_void, payload.error.msg);
    }

    ydebug("ypaint_layer_write: bin payload_len=%zu", payload.value.size);

    struct yetty_ypaint_core_buffer_result res =
        yetty_ypaint_core_buffer_create_from_base64(&payload.value);
    if (YETTY_IS_ERR(res)) {
      yetty_term_osc_args_free(&args);
      return YETTY_ERR(yetty_core_void, res.error.msg);
    }

    struct yetty_core_void_result add_res =
        yetty_ypaint_canvas_add_buffer(layer->canvas, res.value);
    yetty_ypaint_core_buffer_destroy(res.value);
    if (YETTY_IS_ERR(add_res)) {
      yetty_term_osc_args_free(&args);
      return add_res;
    }
  } else {
    ydebug("ypaint_layer_write: unknown format (use --yaml or --bin)");
  }

  yetty_term_osc_args_free(&args);

  layer->base.dirty = 1;
  if (layer->base.request_render_fn)
    layer->base.request_render_fn(layer->base.request_render_userdata);
  return YETTY_OK_VOID();
}

/* Resize */
static struct yetty_core_void_result
ypaint_layer_resize_grid(struct yetty_term_terminal_layer *self,
                         struct grid_size grid_size) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)self;

  if (!layer->canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");

  self->grid_size = grid_size;
  yetty_yetty_ypaint_canvas_set_grid_size(layer->canvas, grid_size);
  self->dirty = 1;

  ydebug("ypaint_layer_resize_grid: %ux%u", grid_size.cols, grid_size.rows);
  return YETTY_OK_VOID();
}

/* Get GPU resource set */
static struct yetty_render_gpu_resource_set_result
ypaint_layer_get_gpu_resource_set(
    const struct yetty_term_terminal_layer *self) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)self;

  if (layer->base.dirty || yetty_yetty_ypaint_canvas_is_dirty(layer->canvas)) {
    /* Rebuild grid staging */
    yetty_yetty_ypaint_canvas_rebuild_grid(layer->canvas);

    const uint32_t *grid_data =
        yetty_yetty_ypaint_canvas_grid_data(layer->canvas);
    uint32_t grid_word_count =
        yetty_yetty_ypaint_canvas_grid_word_count(layer->canvas);

    layer->rs.buffers[0].data = (uint8_t *)grid_data;
    layer->rs.buffers[0].size = grid_word_count * sizeof(uint32_t);
    layer->rs.buffers[0].dirty = 1;

    /* Build primitive staging */
    uint32_t prim_word_count = 0;
    const uint32_t *prim_data = yetty_yetty_ypaint_canvas_build_prim_staging(
        layer->canvas, &prim_word_count);

    layer->rs.buffers[1].data = (uint8_t *)prim_data;
    layer->rs.buffers[1].size = prim_word_count * sizeof(uint32_t);
    layer->rs.buffers[1].dirty = 1;

    /* Update ALL uniforms from canvas - single source of truth */
    struct grid_size gs =
        yetty_yetty_ypaint_canvas_get_grid_size(layer->canvas);
    struct pixel_size cs =
        yetty_yetty_ypaint_canvas_cell_get_pixel_size(layer->canvas);
    set_grid_size(&layer->rs, (float)gs.cols, (float)gs.rows);
    set_cell_size(&layer->rs, cs.width, cs.height);
    set_rolling_row_0(&layer->rs,
                      yetty_yetty_ypaint_canvas_rolling_row_0(layer->canvas));
    uint32_t prim_count =
        yetty_yetty_ypaint_canvas_primitive_count(layer->canvas);
    set_prim_count(&layer->rs, prim_count);

    /* Set pixel size for render target */
    layer->rs.pixel_size.width = (float)gs.cols * cs.width;
    layer->rs.pixel_size.height = (float)gs.rows * cs.height;

    ydebug("ypaint_layer: grid=%ux%u, cell=%.1fx%.1f, prims=%u", gs.cols,
           gs.rows, cs.width, cs.height, prim_count);

    layer->base.dirty = 0;
  }

  /* Include default font as child resource set for glyph rendering */
  struct yetty_font_font *font =
      yetty_yetty_ypaint_canvas_get_default_font(layer->canvas);
  if (font && font->ops && font->ops->get_gpu_resource_set) {
    struct yetty_render_gpu_resource_set_result font_rs =
        font->ops->get_gpu_resource_set(font);
    if (YETTY_IS_OK(font_rs))
      layer->rs.children[0] =
          (struct yetty_render_gpu_resource_set *)font_rs.value;
  }

  return YETTY_OK(yetty_render_gpu_resource_set, &layer->rs);
}

/* Keyboard input - ypaint layer doesn't handle keyboard */
static int ypaint_layer_on_key(struct yetty_term_terminal_layer *self, int key,
                               int mods) {
  (void)self;
  (void)key;
  (void)mods;
  return 0; /* Not handled */
}

static int ypaint_layer_on_char(struct yetty_term_terminal_layer *self,
                                uint32_t codepoint, int mods) {
  (void)self;
  (void)codepoint;
  (void)mods;
  return 0; /* Not handled */
}

/* YPaint layer is empty if there are no primitives */
static int ypaint_layer_is_empty(const struct yetty_term_terminal_layer *self) {
  const struct yetty_term_ypaint_layer *layer =
      (const struct yetty_term_ypaint_layer *)self;

  if (!layer->canvas)
    return 1;

  return yetty_yetty_ypaint_canvas_primitive_count(layer->canvas) == 0;
}

/* Scroll - called when another layer scrolls */
static struct yetty_core_void_result ypaint_layer_scroll(
    struct yetty_term_terminal_layer *self, int lines) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)self;

  ydebug("ypaint_layer_scroll ENTER: lines=%d scrolling_mode=%d canvas=%p",
         lines, layer->scrolling_mode, (void *)layer->canvas);

  if (!layer->canvas)
    return YETTY_ERR(yetty_core_void, "canvas is NULL");
  if (!layer->scrolling_mode || lines <= 0)
    return YETTY_OK_VOID();

  struct yetty_core_void_result res =
      yetty_yetty_ypaint_canvas_scroll_lines(layer->canvas, (uint16_t)lines);
  if (YETTY_IS_ERR(res))
    return res;

  layer->base.dirty = 1;

  ydebug("ypaint_layer_scroll EXIT: %d lines scrolled", lines);

  if (layer->base.request_render_fn)
    layer->base.request_render_fn(layer->base.request_render_userdata);

  return YETTY_OK_VOID();
}

/* Set cursor - called when another layer moves cursor */
static void ypaint_layer_set_cursor(struct yetty_term_terminal_layer *self,
                                    int col, int row) {
  struct yetty_term_ypaint_layer *layer =
      (struct yetty_term_ypaint_layer *)self;

  if (!layer->canvas)
    return;

  yetty_yetty_ypaint_canvas_set_cursor_pos(
      layer->canvas,
      (struct grid_cursor_pos){.cols = (uint32_t)col, .rows = (uint32_t)row});
  ydebug("ypaint_layer_set_cursor: col=%d row=%d", col, row);
}
