#include <yetty/yterm/terminal.h>
#include <yetty/yterm/text-layer.h>
#include <yetty/yterm/ypaint-layer.h>
#include <yetty/yterm/pty-reader.h>
#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/event.h>
#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/pty-poll-source.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/yrender/layer-renderer.h>
#include <yetty/yrender/blender.h>
#include <yetty/yrender/render-target.h>
#include <yetty/yrender/rendered-layer.h>
#include <yetty/ytrace.h>
#include <yetty/yui/view.h>
#include <stdlib.h>
#include <string.h>

#define YETTY_TERM_TERMINAL_MAX_LAYERS 256

/* Forward declarations for view ops */
static void terminal_view_destroy(struct yetty_yui_view *view);
static struct yetty_core_void_result terminal_view_render(
    struct yetty_yui_view *view, void *render_pass);
static struct yetty_core_void_result terminal_view_run(struct yetty_yui_view *view);
static void terminal_view_set_bounds(struct yetty_yui_view *view,
                                     struct yetty_yui_rect bounds);

static const struct yetty_yui_view_ops terminal_view_ops = {
    .destroy = terminal_view_destroy,
    .render = terminal_view_render,
    .run = terminal_view_run,
    .set_bounds = terminal_view_set_bounds,
};

struct yetty_term_terminal {
    struct yetty_yui_view view;  /* MUST be first - allows cast to view */
    struct yetty_core_event_listener listener;
    struct yetty_term_terminal_context context;
    uint32_t cols;
    uint32_t rows;
    struct yetty_term_terminal_layer *layers[YETTY_TERM_TERMINAL_MAX_LAYERS];
    struct yetty_render_layer_renderer *renderers[YETTY_TERM_TERMINAL_MAX_LAYERS];
    size_t layer_count;
    yetty_core_poll_id pty_poll_id;
    struct yetty_render_blender *blender;
    int shutting_down;
    struct yetty_term_pty_reader *pty_reader;
};

/* Forward declarations */
static void terminal_read_pty(struct yetty_term_terminal *terminal);
static struct yetty_core_void_result terminal_render_frame(struct yetty_term_terminal *terminal);

/* PTY write callback for layers */
static void terminal_pty_write_callback(const char *data, size_t len, void *userdata)
{
    struct yetty_term_terminal *terminal = userdata;
    if (terminal->context.pty && terminal->context.pty->ops && terminal->context.pty->ops->write) {
        terminal->context.pty->ops->write(terminal->context.pty, data, len);
        ydebug("terminal_pty_write: wrote %zu bytes to PTY", len);
    }
}

/* Request render callback for layers */
static void terminal_request_render_callback(void *userdata)
{
    struct yetty_term_terminal *terminal = userdata;
    ydebug("terminal_request_render_callback: event_loop=%p", (void*)terminal->context.event_loop);
    if (terminal->context.event_loop && terminal->context.event_loop->ops &&
        terminal->context.event_loop->ops->request_render) {
        ydebug("terminal_request_render_callback: calling request_render");
        terminal->context.event_loop->ops->request_render(terminal->context.event_loop);
    }
}

/* Scroll callback - propagate scroll from source layer to all other layers */
static struct yetty_core_void_result terminal_scroll_callback(
    struct yetty_term_terminal_layer *source, int lines, void *userdata)
{
    struct yetty_term_terminal *terminal = userdata;
    ydebug("terminal_scroll_callback ENTER: source=%p lines=%d layer_count=%zu",
           (void*)source, lines, terminal->layer_count);

    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        if (layer == source)
            continue;
        if (layer->ops && layer->ops->scroll) {
            ydebug("terminal_scroll_callback: calling layer[%zu]=%p scroll(%d)", i, (void*)layer, lines);
            layer->in_external_scroll = 1;
            struct yetty_core_void_result res = layer->ops->scroll(layer, lines);
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
static void terminal_cursor_callback(struct yetty_term_terminal_layer *source,
                                     struct grid_cursor_pos cursor_pos,
                                     void *userdata) {
  struct yetty_term_terminal *terminal = userdata;
  ydebug("terminal_cursor_callback ENTER: source=%p col=%u row=%u layer_count=%zu",
         (void *)source, cursor_pos.cols, cursor_pos.rows, terminal->layer_count);

  for (size_t i = 0; i < terminal->layer_count; i++) {
    struct yetty_term_terminal_layer *layer = terminal->layers[i];
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

/* Event handler */
static int terminal_event_handler(
    struct yetty_core_event_listener *listener,
    const struct yetty_core_event *event)
{
    struct yetty_term_terminal *terminal =
        container_of(listener, struct yetty_term_terminal, listener);

    switch (event->type) {
    case YETTY_EVENT_RENDER: {
        struct yetty_core_void_result res = terminal_render_frame(terminal);
        if (!YETTY_IS_OK(res))
            yerror("terminal: render_frame failed: %s", res.error.msg);
        return 1;
    }

    case YETTY_EVENT_POLL_READABLE:
        ydebug("terminal: POLL_READABLE event fd=%d", event->poll.fd);
        terminal_read_pty(terminal);
        return 1;

    case YETTY_EVENT_KEY_DOWN:
        ydebug("terminal: KEY_DOWN key=%d mods=%d", event->key.key, event->key.mods);
        /* Dispatch to layers */
        for (size_t i = 0; i < terminal->layer_count; i++) {
            struct yetty_term_terminal_layer *layer = terminal->layers[i];
            if (layer && layer->ops && layer->ops->on_key) {
                if (layer->ops->on_key(layer, event->key.key, event->key.mods))
                    break;  /* Event consumed */
            }
        }
        return 1;

    case YETTY_EVENT_CHAR:
        ydebug("terminal: CHAR codepoint=U+%04X mods=%d", event->chr.codepoint, event->chr.mods);
        /* Dispatch to layers */
        for (size_t i = 0; i < terminal->layer_count; i++) {
            struct yetty_term_terminal_layer *layer = terminal->layers[i];
            if (layer && layer->ops && layer->ops->on_char) {
                if (layer->ops->on_char(layer, event->chr.codepoint, event->chr.mods))
                    break;  /* Event consumed */
            }
        }
        return 1;

    case YETTY_EVENT_SHUTDOWN:
        ydebug("terminal: SHUTDOWN received");
        terminal->shutting_down = 1;
        if (terminal->context.event_loop && terminal->context.event_loop->ops &&
            terminal->context.event_loop->ops->stop) {
            terminal->context.event_loop->ops->stop(terminal->context.event_loop);
        }
        return 1;

    case YETTY_EVENT_RESIZE: {
        float width = event->resize.width;
        float height = event->resize.height;
        ydebug("terminal: RESIZE %.0fx%.0f", width, height);

        if (width <= 0 || height <= 0)
            return 1;

        /* Reconfigure WebGPU surface */
        struct yetty_gpu_context *gpu = &terminal->context.yetty_context.gpu_context;
        struct yetty_app_gpu_context *app_gpu = &gpu->app_gpu_context;
        if (app_gpu->surface && gpu->device) {
            WGPUSurfaceConfiguration surface_config = {0};
            surface_config.device = gpu->device;
            surface_config.format = gpu->surface_format;
            surface_config.usage = WGPUTextureUsage_RenderAttachment;
            surface_config.width = (uint32_t)width;
            surface_config.height = (uint32_t)height;
            surface_config.presentMode = WGPUPresentMode_Fifo;
            wgpuSurfaceConfigure(app_gpu->surface, &surface_config);
            app_gpu->surface_width = (uint32_t)width;
            app_gpu->surface_height = (uint32_t)height;
            ydebug("terminal: surface reconfigured to %ux%u", (uint32_t)width, (uint32_t)height);

            /* Resize blender's render target */
            if (terminal->blender && terminal->blender->ops->get_target) {
                struct yetty_render_target *target = terminal->blender->ops->get_target(terminal->blender);
                if (target && target->ops->resize) {
                    target->ops->resize(target, (uint32_t)width, (uint32_t)height);
                }
            }

            /* Resize layer renderers */
            for (size_t i = 0; i < terminal->layer_count; i++) {
                struct yetty_render_layer_renderer *renderer = terminal->renderers[i];
                if (renderer && renderer->ops->resize) {
                    renderer->ops->resize(renderer, (uint32_t)width, (uint32_t)height);
                }
            }
        }

        /* Calculate grid dimensions from first layer's cell size */
        if (terminal->layer_count > 0) {
            struct yetty_term_terminal_layer *layer = terminal->layers[0];
            float cell_w = layer->cell_size.width > 0 ? layer->cell_size.width : 10.0f;
            float cell_h = layer->cell_size.height > 0 ? layer->cell_size.height : 20.0f;
            uint32_t new_cols = (uint32_t)(width / cell_w);
            uint32_t new_rows = (uint32_t)(height / cell_h);

            if (new_cols > 0 && new_rows > 0 &&
                (new_cols != terminal->cols || new_rows != terminal->rows)) {
                ydebug("terminal: resizing grid from %ux%u to %ux%u",
                       terminal->cols, terminal->rows, new_cols, new_rows);
                yetty_term_terminal_resize_grid(terminal,
                    (struct grid_size){.cols = new_cols, .rows = new_rows});

                /* Also resize PTY */
                if (terminal->context.pty && terminal->context.pty->ops &&
                    terminal->context.pty->ops->resize) {
                    terminal->context.pty->ops->resize(terminal->context.pty, new_cols, new_rows);
                }
            }
        }
        return 1;
    }

    default:
        return 0;
    }
}

/* Render a frame using layered rendering */
static struct yetty_core_void_result terminal_render_frame(struct yetty_term_terminal *terminal)
{
    if (terminal->shutting_down) {
        ydebug("terminal_render_frame: shutting down, skipping render");
        return YETTY_OK_VOID();
    }

    if (!terminal->blender) {
        yerror("terminal_render_frame: no blender");
        return YETTY_ERR(yetty_core_void, "no blender");
    }

    ydebug("terminal_render_frame: starting");

    /* Render each layer to texture */
    struct yetty_render_rendered_layer *rendered[YETTY_TERM_TERMINAL_MAX_LAYERS];
    size_t rendered_count = 0;

    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        struct yetty_render_layer_renderer *renderer = terminal->renderers[i];

        if (!layer || !renderer)
            continue;

        struct yetty_render_rendered_layer_result res =
            renderer->ops->render(renderer, layer);

        if (YETTY_IS_OK(res)) {
            rendered[rendered_count++] = res.value;
        } else {
            yerror("terminal_render_frame: layer %zu render failed: %s", i, res.error.msg);
        }
    }

    /* Blend all layers to target */
    struct yetty_core_void_result blend_res =
        terminal->blender->ops->blend(terminal->blender, rendered, rendered_count);

    if (!YETTY_IS_OK(blend_res)) {
        yerror("terminal_render_frame: blend failed: %s", blend_res.error.msg);
        return blend_res;
    }

    ydebug("terminal_render_frame: done, blended %zu layers", rendered_count);
    return YETTY_OK_VOID();
}

/* Read from PTY via pty_reader */
static void terminal_read_pty(struct yetty_term_terminal *terminal)
{
    if (!terminal->pty_reader)
        return;

    int bytes_read = yetty_term_pty_reader_read(terminal->pty_reader);
    if (bytes_read > 0 && terminal->layer_count > 0) {
        struct yetty_term_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->dirty) {
            terminal->context.event_loop->ops->request_render(terminal->context.event_loop);
        }
    }
}

/* Terminal creation/destruction */

struct yetty_term_terminal_result
yetty_term_terminal_create(struct grid_size grid_size,
                           const struct yetty_context *yetty_context) {
  struct yetty_term_terminal *terminal;
  struct yetty_core_void_result res;
  uint32_t cols = grid_size.cols;
  uint32_t rows = grid_size.rows;

  ydebug("terminal_create: cols=%u rows=%u", cols, rows);

  terminal = calloc(1, sizeof(struct yetty_term_terminal));
  if (!terminal)
    return YETTY_ERR(yetty_term_terminal, "failed to allocate terminal");

  /* Initialize view base */
  terminal->view.ops = &terminal_view_ops;
  terminal->view.id = yetty_yui_view_next_id();

  terminal->listener.handler = terminal_event_handler;
  terminal->cols = cols;
  terminal->rows = rows;
    terminal->layer_count = 0;
    terminal->context.yetty_context = *yetty_context;

    /* Create event loop */
    struct yetty_platform_input_pipe *pipe = yetty_context->app_context.platform_input_pipe;
    struct yetty_core_event_loop_result event_loop_res = yetty_core_event_loop_create(pipe);
    if (!YETTY_IS_OK(event_loop_res)) {
        ydebug("terminal_create: failed to create event loop");
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create event loop");
    }
    terminal->context.event_loop = event_loop_res.value;
    ydebug("terminal_create: event_loop created at %p", (void *)terminal->context.event_loop);

    /* Register for render events */
    res = terminal->context.event_loop->ops->register_listener(
        terminal->context.event_loop, YETTY_EVENT_RENDER, &terminal->listener, 0);
    if (!YETTY_IS_OK(res)) {
        ydebug("terminal_create: failed to register render listener");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to register render listener");
    }
    ydebug("terminal_create: registered for RENDER events");

    /* Register for keyboard events */
    res = terminal->context.event_loop->ops->register_listener(
        terminal->context.event_loop, YETTY_EVENT_KEY_DOWN, &terminal->listener, 0);
    if (!YETTY_IS_OK(res)) {
        ydebug("terminal_create: failed to register KEY_DOWN listener");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to register KEY_DOWN listener");
    }
    res = terminal->context.event_loop->ops->register_listener(
        terminal->context.event_loop, YETTY_EVENT_CHAR, &terminal->listener, 0);
    if (!YETTY_IS_OK(res)) {
        ydebug("terminal_create: failed to register CHAR listener");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to register CHAR listener");
    }
    ydebug("terminal_create: registered for keyboard events");

    /* Register for resize events */
    res = terminal->context.event_loop->ops->register_listener(
        terminal->context.event_loop, YETTY_EVENT_RESIZE, &terminal->listener, 0);
    if (!YETTY_IS_OK(res)) {
        ydebug("terminal_create: failed to register RESIZE listener");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to register RESIZE listener");
    }
    ydebug("terminal_create: registered for RESIZE events");

    /* Register for shutdown events */
    res = terminal->context.event_loop->ops->register_listener(
        terminal->context.event_loop, YETTY_EVENT_SHUTDOWN, &terminal->listener, 0);
    if (!YETTY_IS_OK(res)) {
        ydebug("terminal_create: failed to register SHUTDOWN listener");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to register SHUTDOWN listener");
    }
    ydebug("terminal_create: registered for SHUTDOWN events");

    /* Create PTY */
    struct yetty_platform_pty_factory *pty_factory = yetty_context->app_context.pty_factory;
    if (pty_factory && pty_factory->ops && pty_factory->ops->create_pty) {
        struct yetty_platform_pty_result pty_res = pty_factory->ops->create_pty(pty_factory);
        if (YETTY_IS_OK(pty_res)) {
            terminal->context.pty = pty_res.value;
            ydebug("terminal_create: PTY created at %p", (void *)terminal->context.pty);

            /* Create PTY reader */
            struct yetty_term_pty_reader_result reader_res =
                yetty_term_pty_reader_create(terminal->context.pty);
            if (YETTY_IS_OK(reader_res)) {
                terminal->pty_reader = reader_res.value;
                ydebug("terminal_create: pty_reader created");
            }

            /* Set up PTY poll */
            struct yetty_platform_pty_poll_source *poll_source = terminal->context.pty->ops->poll_source(terminal->context.pty);
            if (poll_source) {
                struct yetty_core_poll_id_result poll_res = terminal->context.event_loop->ops->create_pty_poll(
                    terminal->context.event_loop, poll_source);
                if (YETTY_IS_OK(poll_res)) {
                    terminal->pty_poll_id = poll_res.value;
                    terminal->context.event_loop->ops->register_poll_listener(
                        terminal->context.event_loop, terminal->pty_poll_id, &terminal->listener);
                    terminal->context.event_loop->ops->start_poll(
                        terminal->context.event_loop, terminal->pty_poll_id, YETTY_CORE_POLL_READABLE);
                    ydebug("terminal_create: PTY poll started");
                }
            }
        } else {
            ydebug("terminal_create: failed to create PTY (non-fatal)");
        }
    }

    /* Create text layer */
    struct yetty_term_terminal_layer_result text_layer_res = yetty_term_terminal_text_layer_create(
        cols, rows, yetty_context,
        terminal_pty_write_callback, terminal,
        terminal_request_render_callback, terminal,
        terminal_scroll_callback, terminal,
        terminal_cursor_callback, terminal);
    if (!YETTY_IS_OK(text_layer_res)) {
        ydebug("terminal_create: failed to create text layer");
        yetty_term_pty_reader_destroy(terminal->pty_reader);
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create text layer");
    }
    yetty_term_terminal_layer_add(terminal, text_layer_res.value);
    ydebug("terminal_create: text_layer created and added");

    /* Register text layer as default sink for pty_reader */
    if (terminal->pty_reader) {
        yetty_term_pty_reader_register_default_sink(terminal->pty_reader, text_layer_res.value);
        ydebug("terminal_create: text_layer registered as default sink");
    }

    /* Create ypaint scrolling layer (overlay on top of text) */
    {
        struct yetty_term_terminal_layer *text_layer = text_layer_res.value;
        struct yetty_term_terminal_layer_result ypaint_res = yetty_term_ypaint_layer_create(
            cols, rows,
            text_layer->cell_size.width, text_layer->cell_size.height,
            1,  /* scrolling_mode = true */
            yetty_context,
            terminal_request_render_callback, terminal,
            terminal_scroll_callback, terminal,
            terminal_cursor_callback, terminal);
        if (YETTY_IS_OK(ypaint_res)) {
            yetty_term_terminal_layer_add(terminal, ypaint_res.value);
            ydebug("terminal_create: ypaint scrolling layer created and added");

            /* Register ypaint layer for OSC 666674 */
            if (terminal->pty_reader) {
                yetty_term_pty_reader_register_osc_sink(
                    terminal->pty_reader, YETTY_OSC_YPAINT_SCROLL, ypaint_res.value);
                ydebug("terminal_create: ypaint layer registered for OSC 666674");
            }
        } else {
            ydebug("terminal_create: failed to create ypaint layer (non-fatal): %s",
                   ypaint_res.error.msg);
        }
    }

    /* Create render target (surface target) */
    const struct yetty_app_gpu_context *app_gpu = &yetty_context->gpu_context.app_gpu_context;
    struct yetty_render_target_result target_res = yetty_render_target_surface_create(
        yetty_context->gpu_context.device,
        app_gpu->surface,
        yetty_context->gpu_context.surface_format,
        app_gpu->surface_width,
        app_gpu->surface_height);
    if (!YETTY_IS_OK(target_res)) {
        ydebug("terminal_create: failed to create render target");
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create render target");
    }
    ydebug("terminal_create: render target created");

    /* Create blender (takes ownership of target) */
    struct yetty_render_blender_result blender_res = yetty_render_blender_create(
        yetty_context->gpu_context.device,
        yetty_context->gpu_context.queue,
        target_res.value);
    if (!YETTY_IS_OK(blender_res)) {
        ydebug("terminal_create: failed to create blender");
        target_res.value->ops->destroy(target_res.value);
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create blender");
    }
    terminal->blender = blender_res.value;
    ydebug("terminal_create: blender created");

    /* Create layer renderers for each layer */
    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_render_layer_renderer_result renderer_res =
            yetty_render_layer_renderer_create(
                yetty_context->gpu_context.device,
                yetty_context->gpu_context.queue,
                yetty_context->gpu_context.surface_format,
                app_gpu->surface_width,
                app_gpu->surface_height);
        if (!YETTY_IS_OK(renderer_res)) {
            ydebug("terminal_create: failed to create renderer for layer %zu", i);
            /* Clean up already-created renderers */
            for (size_t j = 0; j < i; j++) {
                if (terminal->renderers[j])
                    terminal->renderers[j]->ops->destroy(terminal->renderers[j]);
            }
            terminal->blender->ops->destroy(terminal->blender);
            if (terminal->context.pty)
                terminal->context.pty->ops->destroy(terminal->context.pty);
            terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
            free(terminal);
            return YETTY_ERR(yetty_term_terminal, "failed to create layer renderer");
        }
        terminal->renderers[i] = renderer_res.value;
        ydebug("terminal_create: renderer %zu created", i);
    }

    return YETTY_OK(yetty_term_terminal, terminal);
}

void yetty_term_terminal_destroy(struct yetty_term_terminal *terminal)
{
    size_t i;

    if (!terminal)
        return;

    ydebug("terminal_destroy: starting");

    /* Destroy renderers first */
    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_render_layer_renderer *renderer = terminal->renderers[i];
        if (renderer && renderer->ops && renderer->ops->destroy) {
            ydebug("terminal_destroy: destroying renderer %zu", i);
            renderer->ops->destroy(renderer);
        }
    }
    ydebug("terminal_destroy: renderers destroyed");

    /* Destroy blender (also destroys owned render target) */
    if (terminal->blender && terminal->blender->ops && terminal->blender->ops->destroy) {
        ydebug("terminal_destroy: destroying blender");
        terminal->blender->ops->destroy(terminal->blender);
        ydebug("terminal_destroy: blender destroyed");
    }

    /* Destroy layers */
    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
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

    if (terminal->context.event_loop && terminal->context.event_loop->ops && terminal->context.event_loop->ops->destroy) {
        ydebug("terminal_destroy: destroying event_loop");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
    }

    /* Destroy PTY reader */
    if (terminal->pty_reader) {
        ydebug("terminal_destroy: destroying pty_reader");
        yetty_term_pty_reader_destroy(terminal->pty_reader);
    }

    ydebug("terminal_destroy: freeing terminal struct");
    free(terminal);
    ydebug("terminal_destroy: done");
}

struct yetty_core_void_result yetty_term_terminal_run(struct yetty_term_terminal *terminal)
{
    ydebug("terminal_run: Starting...");

    if (!terminal) {
        ydebug("terminal_run: terminal is null!");
        return YETTY_ERR(yetty_core_void, "terminal is null");
    }

    if (!terminal->context.event_loop) {
        ydebug("terminal_run: terminal has no event_loop!");
        return YETTY_ERR(yetty_core_void, "terminal has no event_loop");
    }

    if (!terminal->context.event_loop->ops) {
        ydebug("terminal_run: event_loop has no ops!");
        return YETTY_ERR(yetty_core_void, "event_loop has no ops");
    }

    if (!terminal->context.event_loop->ops->start) {
        ydebug("terminal_run: event_loop has no start op!");
        return YETTY_ERR(yetty_core_void, "event_loop has no start op");
    }

    ydebug("terminal_run: Calling event_loop start...");
    struct yetty_core_void_result res = terminal->context.event_loop->ops->start(terminal->context.event_loop);
    ydebug("terminal_run: event_loop start returned, ok=%d", YETTY_IS_OK(res));

    if (!YETTY_IS_OK(res)) {
        return res;
    }

    return YETTY_OK_VOID();
}

/* Terminal input */

void yetty_term_terminal_write(struct yetty_term_terminal *terminal,
                               const char *data, size_t len)
{
    if (!terminal || !data || len == 0)
        return;

    /* Send to first layer (text layer) */
    if (terminal->layer_count > 0) {
        struct yetty_term_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->ops && layer->ops->write) {
            layer->ops->write(layer, data, len);
            ydebug("terminal_write: sent %zu bytes to text layer", len);
        }
    }
}

void yetty_term_terminal_resize_grid(struct yetty_term_terminal *terminal,
                                     struct grid_size grid_size) {
  if (!terminal)
    return;

  terminal->cols = grid_size.cols;
  terminal->rows = grid_size.rows;

  for (size_t i = 0; i < terminal->layer_count; i++) {
    struct yetty_term_terminal_layer *layer = terminal->layers[i];
    if (layer && layer->ops && layer->ops->resize_grid)
      layer->ops->resize_grid(layer, grid_size);
  }
}

/* Terminal state */

uint32_t yetty_term_terminal_get_cols(const struct yetty_term_terminal *terminal)
{
    return terminal ? terminal->cols : 0;
}

uint32_t yetty_term_terminal_get_rows(const struct yetty_term_terminal *terminal)
{
    return terminal ? terminal->rows : 0;
}

/* Layer management */

void yetty_term_terminal_layer_add(struct yetty_term_terminal *terminal,
                                   struct yetty_term_terminal_layer *layer)
{
    if (!terminal || !layer)
        return;

    if (terminal->layer_count >= YETTY_TERM_TERMINAL_MAX_LAYERS)
        return;

    terminal->layers[terminal->layer_count++] = layer;
}

void yetty_term_terminal_layer_remove(struct yetty_term_terminal *terminal,
                                      struct yetty_term_terminal_layer *layer)
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

size_t yetty_term_terminal_layer_count(const struct yetty_term_terminal *terminal)
{
    return terminal ? terminal->layer_count : 0;
}

struct yetty_term_terminal_layer *yetty_term_terminal_layer_get(
    const struct yetty_term_terminal *terminal, size_t index)
{
    if (!terminal || index >= terminal->layer_count)
        return NULL;

    return terminal->layers[index];
}

/*=============================================================================
 * View interface implementation
 *===========================================================================*/

struct yetty_yui_view *
yetty_term_terminal_as_view(struct yetty_term_terminal *terminal)
{
    return terminal ? &terminal->view : NULL;
}

static void terminal_view_destroy(struct yetty_yui_view *view)
{
    struct yetty_term_terminal *terminal =
        container_of(view, struct yetty_term_terminal, view);
    yetty_term_terminal_destroy(terminal);
}

static struct yetty_core_void_result terminal_view_render(
    struct yetty_yui_view *view, void *render_pass)
{
    (void)render_pass;  /* Terminal renders via event loop, not direct call */
    struct yetty_term_terminal *terminal =
        container_of(view, struct yetty_term_terminal, view);

    return terminal_render_frame(terminal);
}

static struct yetty_core_void_result terminal_view_run(struct yetty_yui_view *view)
{
    struct yetty_term_terminal *terminal =
        container_of(view, struct yetty_term_terminal, view);

    return yetty_term_terminal_run(terminal);
}

static void terminal_view_set_bounds(struct yetty_yui_view *view,
                                     struct yetty_yui_rect bounds)
{
    struct yetty_term_terminal *terminal =
        container_of(view, struct yetty_term_terminal, view);

    /* Store bounds in view */
    view->bounds = bounds;

    /* Terminal handles resize via YETTY_EVENT_RESIZE from event loop */
    /* For now, just log - the actual resize happens through the event system */
    ydebug("terminal_view_set_bounds: %.0fx%.0f at (%.0f,%.0f)",
           bounds.w, bounds.h, bounds.x, bounds.y);

    (void)terminal;
}
