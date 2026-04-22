#include <yetty/yterm/terminal.h>
#include <yetty/yterm/text-layer.h>
#include <yetty/yterm/ypaint-layer.h>
#include <yetty/yterm/pty-reader.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/event.h>
#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/pty-pipe-source.h>
#include <yetty/yrender/gpu-allocator.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/yrender/render-target.h>
#include <yetty/ytrace.h>
#include <yetty/yui/view.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YETTY_YTERM_TERMINAL_MAX_LAYERS 256

/* Forward declarations for view ops */
static void terminal_view_destroy(struct yetty_yui_view *view);
static struct yetty_ycore_void_result terminal_view_render(
    struct yetty_yui_view *view, struct yetty_yrender_target *render_target);
static void terminal_view_set_bounds(struct yetty_yui_view *view,
                                     struct yetty_yui_rect bounds);
static struct yetty_ycore_int_result terminal_view_on_event(
    struct yetty_yui_view *view, const struct yetty_ycore_event *event);

static const struct yetty_yui_view_ops terminal_view_ops = {
    .destroy = terminal_view_destroy,
    .render = terminal_view_render,
    .set_bounds = terminal_view_set_bounds,
    .on_event = terminal_view_on_event,
};

struct yetty_yterm_terminal {
    struct yetty_yui_view view;  /* MUST be first - allows cast to view */
    struct yetty_ycore_event_listener listener;
    struct yetty_yterm_terminal_context context;
    uint32_t cols;
    uint32_t rows;
    struct yetty_yterm_terminal_layer *layers[YETTY_YTERM_TERMINAL_MAX_LAYERS];
    size_t layer_count;
    yetty_ycore_pipe_id pty_pipe_id;
    /* Render targets - one per layer for render_layer */
    struct yetty_yrender_target *layer_targets[YETTY_YTERM_TERMINAL_MAX_LAYERS];
    int shutting_down;
    struct yetty_yterm_pty_reader *pty_reader;
};

/* Forward declarations */
static void terminal_read_pty(struct yetty_yterm_terminal *terminal);
static struct yetty_ycore_void_result terminal_render_frame(
    struct yetty_yterm_terminal *terminal,
    struct yetty_yrender_target *target);

/* PTY pipe alloc callback — provides buffer for uv_pipe_t reads */
static void terminal_pty_pipe_alloc(void *ctx, size_t suggested_size,
                                     char **buf, size_t *buflen)
{
    (void)ctx;
    (void)suggested_size;
    static char pty_read_buf[8192];
    *buf = pty_read_buf;
    *buflen = sizeof(pty_read_buf);
}

/* PTY pipe read callback — feeds data to pty_reader, triggers render */
static void terminal_pty_pipe_read(void *ctx, const char *buf, long nread)
{
    struct yetty_yterm_terminal *terminal = ctx;

    if (nread > 0 && terminal->pty_reader) {
        yetty_yterm_pty_reader_feed(terminal->pty_reader, buf, (size_t)nread);
        if (terminal->layer_count > 0) {
            struct yetty_yterm_terminal_layer *layer = terminal->layers[0];
            if (layer && layer->dirty) {
                terminal->context.yetty_context.event_loop->ops->request_render(
                    terminal->context.yetty_context.event_loop);
            }
        }
    }
}

/* PTY write callback for layers */
static void terminal_pty_write_callback(const char *data, size_t len, void *userdata)
{
    struct yetty_yterm_terminal *terminal = userdata;
    if (terminal->context.pty && terminal->context.pty->ops && terminal->context.pty->ops->write) {
        terminal->context.pty->ops->write(terminal->context.pty, data, len);
        ydebug("terminal_pty_write: wrote %zu bytes to PTY", len);
    }
}

/* Request render callback for layers */
static void terminal_request_render_callback(void *userdata)
{
    struct yetty_yterm_terminal *terminal = userdata;
    ydebug("terminal_request_render_callback: event_loop=%p", (void*)terminal->context.yetty_context.event_loop);
    if (terminal->context.yetty_context.event_loop && terminal->context.yetty_context.event_loop->ops &&
        terminal->context.yetty_context.event_loop->ops->request_render) {
        ydebug("terminal_request_render_callback: calling request_render");
        terminal->context.yetty_context.event_loop->ops->request_render(terminal->context.yetty_context.event_loop);
    }
}

/* Scroll callback - propagate scroll from source layer to all other layers */
static struct yetty_ycore_void_result terminal_scroll_callback(
    struct yetty_yterm_terminal_layer *source, int lines, void *userdata)
{
    struct yetty_yterm_terminal *terminal = userdata;
    ydebug("terminal_scroll_callback ENTER: source=%p lines=%d layer_count=%zu",
           (void*)source, lines, terminal->layer_count);

    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
        if (layer == source)
            continue;
        if (layer->ops && layer->ops->scroll) {
            ydebug("terminal_scroll_callback: calling layer[%zu]=%p scroll(%d)", i, (void*)layer, lines);
            layer->in_external_scroll = 1;
            struct yetty_ycore_void_result res = layer->ops->scroll(layer, lines);
            layer->in_external_scroll = 0;
            if (YETTY_IS_ERR(res)) {
                yerror("terminal_scroll_callback: layer[%zu] scroll failed: %s", i, res.error.msg);
                return res;
            }
        }
    }
    ydebug("terminal_scroll_callback EXIT: lines=%d", lines);
    return YETTY_OK_VOID();
}

/* Cursor callback - propagate cursor position from source layer to all other layers */
static void terminal_cursor_callback(struct yetty_yterm_terminal_layer *source,
                                     struct grid_cursor_pos cursor_pos,
                                     void *userdata) {
  struct yetty_yterm_terminal *terminal = userdata;
  ydebug("terminal_cursor_callback ENTER: source=%p col=%u row=%u layer_count=%zu",
         (void *)source, cursor_pos.cols, cursor_pos.rows, terminal->layer_count);

  for (size_t i = 0; i < terminal->layer_count; i++) {
    struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
    if (layer != source && layer->ops && layer->ops->set_cursor) {
      ydebug("terminal_cursor_callback: calling layer[%zu]=%p set_cursor(%u,%u)",
             i, (void *)layer, cursor_pos.cols, cursor_pos.rows);
      layer->ops->set_cursor(layer, cursor_pos.cols, cursor_pos.rows);
    } else {
      ydebug("terminal_cursor_callback: skipping layer[%zu]=%p (source=%d "
             "has_set_cursor=%d)",
             i, (void *)layer, layer == source,
             layer->ops && layer->ops->set_cursor);
    }
  }
  ydebug("terminal_cursor_callback EXIT: col=%u row=%u", cursor_pos.cols,
         cursor_pos.rows);
}

/* Event handler - only for PTY poll events registered directly with event loop */
static struct yetty_ycore_int_result terminal_event_handler(
    struct yetty_ycore_event_listener *listener,
    const struct yetty_ycore_event *event)
{
    struct yetty_yterm_terminal *terminal =
        container_of(listener, struct yetty_yterm_terminal, listener);

    /* PTY data now arrives via uv_pipe_t read callback, not through events */
    (void)terminal;
    (void)event;

    return YETTY_OK(yetty_ycore_int, 0);
}

/* Render a frame using layered rendering */
static struct yetty_ycore_void_result terminal_render_frame(
    struct yetty_yterm_terminal *terminal,
    struct yetty_yrender_target *target)
{
    if (terminal->shutting_down) {
        ydebug("terminal_render_frame: shutting down, skipping render");
        return YETTY_OK_VOID();
    }

    if (!target) {
        yerror("terminal_render_frame: no target provided");
        return YETTY_ERR(yetty_ycore_void, "no target provided");
    }

    ydebug("terminal_render_frame: starting");

    /* Render each layer to its target */
    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
        struct yetty_yrender_target *layer_target = terminal->layer_targets[i];

        if (!layer || !layer_target)
            continue;

        struct yetty_ycore_void_result res =
            layer_target->ops->render_layer(layer_target, layer);

        if (!YETTY_IS_OK(res)) {
            yerror("terminal_render_frame: layer %zu render failed: %s",
                   i, res.error.msg);
            return res;
        }
    }

    /* Blend all layer targets into the provided target (big_target from yetty) */
    struct yetty_ycore_void_result res =
        target->ops->blend(target, terminal->layer_targets, terminal->layer_count);

    if (!YETTY_IS_OK(res)) {
        yerror("terminal_render_frame: blend failed: %s", res.error.msg);
        return res;
    }

    ydebug("terminal_render_frame: done, rendered %zu layers", terminal->layer_count);
    return YETTY_OK_VOID();
}

/* Read from PTY via pty_reader */
static void terminal_read_pty(struct yetty_yterm_terminal *terminal)
{
    if (!terminal->pty_reader)
        return;

    int bytes_read = yetty_yterm_pty_reader_read(terminal->pty_reader);
    if (bytes_read > 0 && terminal->layer_count > 0) {
        struct yetty_yterm_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->dirty) {
            terminal->context.yetty_context.event_loop->ops->request_render(terminal->context.yetty_context.event_loop);
        }
    }
}

/* Terminal creation/destruction */

struct yetty_yterm_terminal_result
yetty_yterm_terminal_create(struct grid_size grid_size,
                           const struct yetty_context *yetty_context) {
  struct yetty_yterm_terminal *terminal;
  uint32_t cols = grid_size.cols;
  uint32_t rows = grid_size.rows;

  ydebug("terminal_create: cols=%u rows=%u", cols, rows);

  terminal = calloc(1, sizeof(struct yetty_yterm_terminal));
  if (!terminal)
    return YETTY_ERR(yetty_yterm_terminal, "failed to allocate terminal");

  /* Initialize view base */
  terminal->view.ops = &terminal_view_ops;
  terminal->view.id = yetty_yui_view_next_id();

  terminal->cols = cols;
  terminal->rows = rows;
    terminal->layer_count = 0;
    terminal->context.yetty_context = *yetty_context;

    /* Validate event loop from context */
    if (!yetty_context->event_loop) {
        ydebug("terminal_create: no event_loop in context");
        free(terminal);
        return YETTY_ERR(yetty_yterm_terminal, "no event_loop in context");
    }
    ydebug("terminal_create: using event_loop at %p",
           (void *)terminal->context.yetty_context.event_loop);

    /* Set up listener for PTY poll events */
    terminal->listener.handler = terminal_event_handler;

    /* Create PTY */
    struct yetty_platform_pty_factory *pty_factory = yetty_context->app_context.pty_factory;
    if (pty_factory && pty_factory->ops && pty_factory->ops->create_pty) {
        struct yetty_platform_pty_result pty_res = pty_factory->ops->create_pty(pty_factory);
        if (YETTY_IS_OK(pty_res)) {
            terminal->context.pty = pty_res.value;
            ydebug("terminal_create: PTY created at %p", (void *)terminal->context.pty);

            /* Create PTY reader */
            struct yetty_yterm_pty_reader_result reader_res =
                yetty_yterm_pty_reader_create(terminal->context.pty);
            if (YETTY_IS_OK(reader_res)) {
                terminal->pty_reader = reader_res.value;
                ydebug("terminal_create: pty_reader created");
            }

            /* Register PTY pipe — uv_pipe_t reads data, callbacks handle it */
            struct yetty_platform_pty_pipe_source *pipe_source =
                terminal->context.pty->ops->pipe_source(terminal->context.pty);
            if (pipe_source && terminal->pty_reader) {
                struct yetty_ycore_pipe_id_result pipe_res =
                    terminal->context.yetty_context.event_loop->ops->register_pty_pipe(
                        terminal->context.yetty_context.event_loop, pipe_source,
                        terminal_pty_pipe_alloc, terminal_pty_pipe_read, terminal);
                if (YETTY_IS_OK(pipe_res)) {
                    terminal->pty_pipe_id = pipe_res.value;
                    ydebug("terminal_create: PTY pipe registered");
                }
            }
        } else {
            ydebug("terminal_create: failed to create PTY (non-fatal)");
        }
    }

    /* Create text layer */
    struct yetty_yterm_terminal_layer_result text_layer_res = yetty_yterm_terminal_text_layer_create(
        cols, rows, yetty_context,
        terminal_pty_write_callback, terminal,
        terminal_request_render_callback, terminal,
        terminal_scroll_callback, terminal,
        terminal_cursor_callback, terminal);
    if (!YETTY_IS_OK(text_layer_res)) {
        yerror("terminal_create: failed to create text layer: %s", text_layer_res.error.msg);
        yetty_yterm_pty_reader_destroy(terminal->pty_reader);
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        free(terminal);
        return YETTY_ERR(yetty_yterm_terminal, text_layer_res.error.msg);
    }
    yetty_yterm_terminal_layer_add(terminal, text_layer_res.value);
    ydebug("terminal_create: text_layer created and added");

    /* Register text layer as default sink for pty_reader */
    if (terminal->pty_reader) {
        yetty_yterm_pty_reader_register_default_sink(terminal->pty_reader, text_layer_res.value);
        ydebug("terminal_create: text_layer registered as default sink");
    }

    /* Create ypaint scrolling layer (overlay on top of text) */
    {
        struct yetty_yterm_terminal_layer *text_layer = text_layer_res.value;
        struct yetty_yterm_terminal_layer_result ypaint_res = yetty_yterm_ypaint_layer_create(
            cols, rows,
            text_layer->cell_size.width, text_layer->cell_size.height,
            1,  /* scrolling_mode = true */
            yetty_context,
            terminal_request_render_callback, terminal,
            terminal_scroll_callback, terminal,
            terminal_cursor_callback, terminal);
        if (YETTY_IS_OK(ypaint_res)) {
            yetty_yterm_terminal_layer_add(terminal, ypaint_res.value);
            ydebug("terminal_create: ypaint scrolling layer created and added");

            /* Register ypaint layer for OSC 666674 */
            if (terminal->pty_reader) {
                yetty_yterm_pty_reader_register_osc_sink(
                    terminal->pty_reader, YETTY_OSC_YPAINT_SCROLL, ypaint_res.value);
                ydebug("terminal_create: ypaint layer registered for OSC 666674");
            }
        } else {
            ydebug("terminal_create: failed to create ypaint layer (non-fatal): %s",
                   ypaint_res.error.msg);
        }
    }

    /* Create render targets for each layer */
    const struct yetty_app_gpu_context *app_gpu = &yetty_context->gpu_context.app_gpu_context;
    struct yetty_yrender_viewport layer_vp = {
        .x = 0, .y = 0,
        .w = (float)app_gpu->surface_width,
        .h = (float)app_gpu->surface_height
    };
    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_yrender_target_ptr_result target_res = yetty_yrender_target_texture_create(
            yetty_context->gpu_context.device,
            yetty_context->gpu_context.queue,
            yetty_context->gpu_context.surface_format,
            yetty_context->gpu_context.allocator,
            NULL,  /* no surface for layer targets */
            layer_vp);
        if (!YETTY_IS_OK(target_res)) {
            ydebug("terminal_create: failed to create layer target %zu", i);
            /* Clean up already created targets */
            for (size_t j = 0; j < i; j++) {
                if (terminal->layer_targets[j])
                    terminal->layer_targets[j]->ops->destroy(terminal->layer_targets[j]);
            }
            if (terminal->context.pty)
                terminal->context.pty->ops->destroy(terminal->context.pty);
            free(terminal);
            return YETTY_ERR(yetty_yterm_terminal, "failed to create layer target");
        }
        terminal->layer_targets[i] = target_res.value;
    }
    ydebug("terminal_create: layer targets created");

    return YETTY_OK(yetty_yterm_terminal, terminal);
}

void yetty_yterm_terminal_destroy(struct yetty_yterm_terminal *terminal)
{
    size_t i;

    if (!terminal)
        return;

    ydebug("terminal_destroy: starting");

    /* Destroy layer targets */
    for (size_t i = 0; i < terminal->layer_count; i++) {
        if (terminal->layer_targets[i] && terminal->layer_targets[i]->ops &&
            terminal->layer_targets[i]->ops->destroy) {
            ydebug("terminal_destroy: destroying layer_target %zu", i);
            terminal->layer_targets[i]->ops->destroy(terminal->layer_targets[i]);
        }
    }
    ydebug("terminal_destroy: layer_targets destroyed");

    /* Destroy layers */
    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
        if (layer && layer->ops && layer->ops->destroy) {
            ydebug("terminal_destroy: destroying layer %zu", i);
            layer->ops->destroy(layer);
        }
    }
    ydebug("terminal_destroy: layers destroyed");

    if (terminal->context.pty && terminal->context.pty->ops && terminal->context.pty->ops->destroy) {
        ydebug("terminal_destroy: destroying pty");
        terminal->context.pty->ops->destroy(terminal->context.pty);
    }

    /* event_loop is owned by yetty, not terminal - do not destroy */

    /* Destroy PTY reader */
    if (terminal->pty_reader) {
        ydebug("terminal_destroy: destroying pty_reader");
        yetty_yterm_pty_reader_destroy(terminal->pty_reader);
    }

    ydebug("terminal_destroy: freeing terminal struct");
    free(terminal);
    ydebug("terminal_destroy: done");
}

/* Terminal input */

void yetty_yterm_terminal_write(struct yetty_yterm_terminal *terminal,
                               const char *data, size_t len)
{
    if (!terminal || !data || len == 0)
        return;

    /* Send to first layer (text layer) */
    if (terminal->layer_count > 0) {
        struct yetty_yterm_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->ops && layer->ops->write) {
            layer->ops->write(layer, data, len);
            ydebug("terminal_write: sent %zu bytes to text layer", len);
        }
    }
}

void yetty_yterm_terminal_resize_grid(struct yetty_yterm_terminal *terminal,
                                     struct grid_size grid_size) {
  if (!terminal)
    return;

  terminal->cols = grid_size.cols;
  terminal->rows = grid_size.rows;

  for (size_t i = 0; i < terminal->layer_count; i++) {
    struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
    if (layer && layer->ops && layer->ops->resize_grid)
      layer->ops->resize_grid(layer, grid_size);
  }
}

/* Terminal state */

uint32_t yetty_yterm_terminal_get_cols(const struct yetty_yterm_terminal *terminal)
{
    return terminal ? terminal->cols : 0;
}

uint32_t yetty_yterm_terminal_get_rows(const struct yetty_yterm_terminal *terminal)
{
    return terminal ? terminal->rows : 0;
}

/* Layer management */

void yetty_yterm_terminal_layer_add(struct yetty_yterm_terminal *terminal,
                                   struct yetty_yterm_terminal_layer *layer)
{
    if (!terminal || !layer)
        return;

    if (terminal->layer_count >= YETTY_YTERM_TERMINAL_MAX_LAYERS)
        return;

    terminal->layers[terminal->layer_count++] = layer;
}

void yetty_yterm_terminal_layer_remove(struct yetty_yterm_terminal *terminal,
                                      struct yetty_yterm_terminal_layer *layer)
{
    size_t i;

    if (!terminal || !layer)
        return;

    for (i = 0; i < terminal->layer_count; i++) {
        if (terminal->layers[i] == layer) {
            memmove(&terminal->layers[i], &terminal->layers[i + 1],
                    (terminal->layer_count - i - 1) * sizeof(terminal->layers[0]));
            terminal->layer_count--;
            return;
        }
    }
}

size_t yetty_yterm_terminal_layer_count(const struct yetty_yterm_terminal *terminal)
{
    return terminal ? terminal->layer_count : 0;
}

struct yetty_yterm_terminal_layer *yetty_yterm_terminal_layer_get(
    const struct yetty_yterm_terminal *terminal, size_t index)
{
    if (!terminal || index >= terminal->layer_count)
        return NULL;

    return terminal->layers[index];
}

/*=============================================================================
 * View interface implementation
 *===========================================================================*/

struct yetty_yui_view *
yetty_yterm_terminal_as_view(struct yetty_yterm_terminal *terminal)
{
    return terminal ? &terminal->view : NULL;
}

static void terminal_view_destroy(struct yetty_yui_view *view)
{
    struct yetty_yterm_terminal *terminal =
        container_of(view, struct yetty_yterm_terminal, view);
    yetty_yterm_terminal_destroy(terminal);
}

static struct yetty_ycore_void_result terminal_view_render(
    struct yetty_yui_view *view, struct yetty_yrender_target *render_target)
{
    struct yetty_yterm_terminal *terminal =
        container_of(view, struct yetty_yterm_terminal, view);

    return terminal_render_frame(terminal, render_target);
}

static void terminal_view_set_bounds(struct yetty_yui_view *view,
                                     struct yetty_yui_rect bounds)
{
    struct yetty_yterm_terminal *terminal =
        container_of(view, struct yetty_yterm_terminal, view);

    /* Store bounds in view */
    view->bounds = bounds;

    /* Terminal handles resize via YETTY_EVENT_RESIZE from event loop */
    /* For now, just log - the actual resize happens through the event system */
    ydebug("terminal_view_set_bounds: %.0fx%.0f at (%.0f,%.0f)",
           bounds.w, bounds.h, bounds.x, bounds.y);

    (void)terminal;
}

static struct yetty_ycore_int_result terminal_view_on_event(
    struct yetty_yui_view *view, const struct yetty_ycore_event *event)
{
    struct yetty_yterm_terminal *terminal =
        container_of(view, struct yetty_yterm_terminal, view);

    switch (event->type) {
    case YETTY_EVENT_KEY_DOWN:
        ydebug("terminal: KEY_DOWN key=%d mods=%d", event->key.key, event->key.mods);
        for (size_t i = 0; i < terminal->layer_count; i++) {
            struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
            if (layer && layer->ops && layer->ops->on_key) {
                if (layer->ops->on_key(layer, event->key.key, event->key.mods))
                    return YETTY_OK(yetty_ycore_int, 1);
            }
        }
        return YETTY_OK(yetty_ycore_int, 1);

    case YETTY_EVENT_CHAR:
        ydebug("terminal: CHAR codepoint=U+%04X mods=%d", event->chr.codepoint, event->chr.mods);
        for (size_t i = 0; i < terminal->layer_count; i++) {
            struct yetty_yterm_terminal_layer *layer = terminal->layers[i];
            if (layer && layer->ops && layer->ops->on_char) {
                if (layer->ops->on_char(layer, event->chr.codepoint, event->chr.mods))
                    return YETTY_OK(yetty_ycore_int, 1);
            }
        }
        return YETTY_OK(yetty_ycore_int, 1);

    case YETTY_EVENT_RESIZE: {
        float width = event->resize.width;
        float height = event->resize.height;
        ydebug("terminal: RESIZE %.0fx%.0f", width, height);

        if (width <= 0 || height <= 0)
            return YETTY_OK(yetty_ycore_int, 1);

        /* Resize layer targets - yetty handles surface reconfiguration */
        struct yetty_yrender_viewport vp = {
            .x = 0, .y = 0,
            .w = width, .h = height
        };
        for (size_t i = 0; i < terminal->layer_count; i++) {
            if (terminal->layer_targets[i] && terminal->layer_targets[i]->ops->resize) {
                terminal->layer_targets[i]->ops->resize(terminal->layer_targets[i], vp);
            }
        }

        /* Calculate grid dimensions from first layer's cell size */
        if (terminal->layer_count > 0) {
            struct yetty_yterm_terminal_layer *layer = terminal->layers[0];
            float cell_w = layer->cell_size.width > 0 ? layer->cell_size.width : 10.0f;
            float cell_h = layer->cell_size.height > 0 ? layer->cell_size.height : 20.0f;
            uint32_t new_cols = (uint32_t)(width / cell_w);
            uint32_t new_rows = (uint32_t)(height / cell_h);

            if (new_cols > 0 && new_rows > 0 &&
                (new_cols != terminal->cols || new_rows != terminal->rows)) {
                yetty_yterm_terminal_resize_grid(terminal,
                    (struct grid_size){.cols = new_cols, .rows = new_rows});
                if (terminal->context.pty && terminal->context.pty->ops &&
                    terminal->context.pty->ops->resize) {
                    terminal->context.pty->ops->resize(terminal->context.pty, new_cols, new_rows);
                }
            }
        }
        return YETTY_OK(yetty_ycore_int, 1);
    }

    case YETTY_EVENT_SHUTDOWN:
        ydebug("terminal: SHUTDOWN received");
        terminal->shutting_down = 1;
        return YETTY_OK(yetty_ycore_int, 1);

    case YETTY_EVENT_POLL_READABLE:
        ydebug("terminal: POLL_READABLE");
        terminal_read_pty(terminal);
        return YETTY_OK(yetty_ycore_int, 1);

    default:
        return YETTY_OK(yetty_ycore_int, 0);
    }
}
