#include <yetty/term/terminal.h>
#include <yetty/term/text-layer.h>
#include <yetty/core/event-loop.h>
#include <yetty/core/event.h>
#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/pty-poll-source.h>
#include <yetty/render/gpu-resource-set.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define YETTY_TERM_TERMINAL_MAX_LAYERS 256

struct yetty_term_terminal {
    struct yetty_core_event_listener listener;  /* must be first for container_of */
    struct yetty_term_terminal_context context;
    uint32_t cols;
    uint32_t rows;
    struct yetty_term_terminal_layer *layers[YETTY_TERM_TERMINAL_MAX_LAYERS];
    size_t layer_count;
    yetty_core_poll_id pty_poll_id;
};

/* Forward declaration */
static void terminal_read_pty(struct yetty_term_terminal *terminal);

/* Event handler */
static int terminal_event_handler(
    struct yetty_core_event_listener *listener,
    const struct yetty_core_event *event)
{
    struct yetty_term_terminal *terminal = (struct yetty_term_terminal *)listener;

    switch (event->type) {
    case YETTY_EVENT_RENDER:
        ydebug("terminal: RENDER event, layer_count=%zu", terminal->layer_count);
        for (size_t i = 0; i < terminal->layer_count; i++) {
            struct yetty_term_terminal_layer *layer = terminal->layers[i];
            if (layer && layer->ops && layer->ops->get_gpu_resource_set) {
                struct yetty_render_gpu_resource_set rs = layer->ops->get_gpu_resource_set(layer);
                ydebug("terminal: layer %zu resource_set name=%s buffer_size=%zu shader_size=%zu",
                       i, rs.name, rs.buffer_size, rs.shader_code_size);
                /* TODO: actually render using the resource set */
            }
        }
        return 1;

    case YETTY_EVENT_POLL_READABLE:
        ydebug("terminal: POLL_READABLE event fd=%d", event->poll.fd);
        terminal_read_pty(terminal);
        return 1;

    default:
        return 0;
    }
}

/* Read from PTY and feed to text layer */
static void terminal_read_pty(struct yetty_term_terminal *terminal)
{
    struct yetty_platform_pty *pty = terminal->context.pty;
    char buf[4096];
    int dirty = 0;

    if (!pty || !pty->ops || !pty->ops->read)
        return;

    struct yetty_core_size_result res;
    while ((res = pty->ops->read(pty, buf, sizeof(buf))), YETTY_IS_OK(res) && res.value > 0) {
        /* Feed to text layer */
        if (terminal->layer_count > 0) {
            struct yetty_term_terminal_layer *layer = terminal->layers[0];
            if (layer && layer->ops && layer->ops->write) {
                layer->ops->write(layer, buf, res.value);
                dirty = 1;
            }
        }
    }

    /* Request render if text layer is dirty */
    if (dirty && terminal->layer_count > 0) {
        struct yetty_term_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->dirty) {
            terminal->context.event_loop->ops->request_render(terminal->context.event_loop);
        }
    }
}

/* Terminal creation/destruction */

struct yetty_term_terminal_result yetty_term_terminal_create(
    uint32_t cols, uint32_t rows,
    const struct yetty_context *yetty_context)
{
    struct yetty_term_terminal *terminal;
    struct yetty_core_void_result res;

    ydebug("terminal_create: cols=%u rows=%u", cols, rows);

    terminal = calloc(1, sizeof(struct yetty_term_terminal));
    if (!terminal)
        return YETTY_ERR(yetty_term_terminal, "failed to allocate terminal");

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

    /* Create PTY */
    struct yetty_platform_pty_factory *pty_factory = yetty_context->app_context.pty_factory;
    if (pty_factory && pty_factory->ops && pty_factory->ops->create_pty) {
        struct yetty_platform_pty_result pty_res = pty_factory->ops->create_pty(pty_factory);
        if (YETTY_IS_OK(pty_res)) {
            terminal->context.pty = pty_res.value;
            ydebug("terminal_create: PTY created at %p", (void *)terminal->context.pty);

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
    struct yetty_term_terminal_layer_result text_layer_res = yetty_term_terminal_text_layer_create(cols, rows);
    if (!YETTY_IS_OK(text_layer_res)) {
        ydebug("terminal_create: failed to create text layer");
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create text layer");
    }
    yetty_term_terminal_layer_add(terminal, text_layer_res.value);
    ydebug("terminal_create: text_layer created and added");

    return YETTY_OK(yetty_term_terminal, terminal);
}

void yetty_term_terminal_destroy(struct yetty_term_terminal *terminal)
{
    size_t i;

    if (!terminal)
        return;

    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        if (layer && layer->ops && layer->ops->destroy)
            layer->ops->destroy(layer);
    }

    if (terminal->context.pty && terminal->context.pty->ops && terminal->context.pty->ops->destroy)
        terminal->context.pty->ops->destroy(terminal->context.pty);

    if (terminal->context.event_loop && terminal->context.event_loop->ops && terminal->context.event_loop->ops->destroy)
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);

    free(terminal);
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

void yetty_term_terminal_resize(struct yetty_term_terminal *terminal,
                                uint32_t cols, uint32_t rows)
{
    size_t i;

    if (!terminal)
        return;

    terminal->cols = cols;
    terminal->rows = rows;

    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        if (layer && layer->ops && layer->ops->resize)
            layer->ops->resize(layer, cols, rows);
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
