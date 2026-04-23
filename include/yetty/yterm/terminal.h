#ifndef YETTY_YTERM_TERMINAL_H
#define YETTY_YTERM_TERMINAL_H

#include <stddef.h>
#include <stdint.h>
#include <webgpu/webgpu.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/yetty.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct yetty_yterm_terminal;
struct yetty_yterm_terminal_layer;
struct yetty_yterm_terminal_layer_ops;
struct yetty_ycore_event_loop;
struct yetty_yplatform_pty;
struct yetty_yui_view;
struct yetty_yrender_gpu_resource_binder;

/* Render layer function - stateless, renders layer to target texture.
 * Returns early with OK if layer is not dirty. */
typedef struct yetty_ycore_void_result (*yetty_yrender_layer_fn)(
    struct yetty_yterm_terminal_layer *layer,
    WGPUTextureView target,
    struct yetty_yrender_gpu_resource_binder *binder,
    struct yetty_gpu_context *gpu);

/* Result types */
YETTY_YRESULT_DECLARE(yetty_yterm_terminal, struct yetty_yterm_terminal *);
YETTY_YRESULT_DECLARE(yetty_yterm_terminal_layer,
                     struct yetty_yterm_terminal_layer *);

/* PTY write callback - called when layer needs to send data to PTY */
typedef void (*yetty_yterm_pty_write_fn)(const char *data, size_t len,
                                        void *userdata);

/* Request render callback - called when layer needs a render frame */
typedef void (*yetty_yterm_request_render_fn)(void *userdata);

/* Scroll callback - called when layer scrolls, passes source layer and line
 * count */
typedef struct yetty_ycore_void_result (*yetty_yterm_scroll_fn)(
    struct yetty_yterm_terminal_layer *source, int lines, void *userdata);

/* Cursor callback - called when layer moves cursor, passes source layer and
 * position */
typedef void (*yetty_yterm_cursor_fn)(struct yetty_yterm_terminal_layer *source,
                                     struct grid_cursor_pos cursor_pos,
                                     void *userdata);

/* Layer ops */
struct yetty_yterm_terminal_layer_ops {
  void (*destroy)(struct yetty_yterm_terminal_layer *self);
  struct yetty_ycore_void_result (*write)(struct yetty_yterm_terminal_layer *self,
                                         const char *data, size_t len);
  struct yetty_ycore_void_result (*resize_grid)(
      struct yetty_yterm_terminal_layer *self, struct grid_size grid_size);
  struct yetty_yrender_gpu_resource_set_result (*get_gpu_resource_set)(
      const struct yetty_yterm_terminal_layer *self);
  /* Returns 1 if layer has no content to render (skip rendering, use
   * transparent texture) */
  int (*is_empty)(const struct yetty_yterm_terminal_layer *self);
  /* Keyboard input - returns 1 if handled */
  int (*on_key)(struct yetty_yterm_terminal_layer *self, int key, int mods);
  int (*on_char)(struct yetty_yterm_terminal_layer *self, uint32_t codepoint,
                 int mods);
  /* Scroll - called when another layer scrolls, lines > 0 = scroll down */
  struct yetty_ycore_void_result (*scroll)(struct yetty_yterm_terminal_layer *self, int lines);
  /* Cursor - called when another layer moves cursor */
  void (*set_cursor)(struct yetty_yterm_terminal_layer *self, int col, int row);
};

/* Layer base - embed as first member in subclasses */
struct yetty_yterm_terminal_layer {
  const struct yetty_yterm_terminal_layer_ops *ops;
  struct grid_size grid_size;
  struct pixel_size cell_size;

  int dirty;
  int in_external_scroll; /* Set when receiving scroll from another layer */
  /* PTY write callback - set by creator */
  yetty_yterm_pty_write_fn pty_write_fn;
  void *pty_write_userdata;
  /* Request render callback - set by creator */
  yetty_yterm_request_render_fn request_render_fn;
  void *request_render_userdata;
  /* Scroll callback - set by creator */
  yetty_yterm_scroll_fn scroll_fn;
  void *scroll_userdata;
  /* Cursor callback - set by creator */
  yetty_yterm_cursor_fn cursor_fn;
  void *cursor_userdata;
};

/* Terminal context - contains yetty context plus terminal-owned objects */
struct yetty_yterm_terminal_context {
  struct yetty_context yetty_context;
  struct yetty_yplatform_pty *pty;
};

/* Terminal creation/destruction */
struct yetty_yterm_terminal_result
yetty_yterm_terminal_create(struct grid_size grid_size,
                           const struct yetty_context *yetty_context);
void yetty_yterm_terminal_destroy(struct yetty_yterm_terminal *terminal);

/* Get terminal as yui view (for pushing into pane) */
struct yetty_yui_view *
yetty_yterm_terminal_as_view(struct yetty_yterm_terminal *terminal);

/* Terminal input */
void yetty_yterm_terminal_write(struct yetty_yterm_terminal *terminal,
                               const char *data, size_t len);

void yetty_yterm_terminal_resize_grid(struct yetty_yterm_terminal *terminal,
                                     struct grid_size grid_size);

/* Terminal state */
uint32_t
yetty_yterm_terminal_get_cols(const struct yetty_yterm_terminal *terminal);
uint32_t
yetty_yterm_terminal_get_rows(const struct yetty_yterm_terminal *terminal);

/* Layer management */
void yetty_yterm_terminal_layer_add(struct yetty_yterm_terminal *terminal,
                                   struct yetty_yterm_terminal_layer *layer);
void yetty_yterm_terminal_layer_remove(struct yetty_yterm_terminal *terminal,
                                      struct yetty_yterm_terminal_layer *layer);
size_t
yetty_yterm_terminal_layer_count(const struct yetty_yterm_terminal *terminal);
struct yetty_yterm_terminal_layer *
yetty_yterm_terminal_layer_get(const struct yetty_yterm_terminal *terminal,
                              size_t index);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YTERM_TERMINAL_H */
