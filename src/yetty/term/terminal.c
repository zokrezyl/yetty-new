#include <yetty/term/terminal.h>
#include <yetty/core/event-loop.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define YETTY_TERM_TERMINAL_MAX_LAYERS 256

struct yetty_term_terminal {
    uint32_t cols;
    uint32_t rows;
    struct yetty_term_terminal_layer *layers[YETTY_TERM_TERMINAL_MAX_LAYERS];
    size_t layer_count;
    struct yetty_core_event_loop *event_loop;
};

/* Terminal creation/destruction */

struct yetty_term_terminal_result yetty_term_terminal_create(
    uint32_t cols, uint32_t rows,
    struct yetty_platform_input_pipe *platform_input_pipe)
{
    struct yetty_term_terminal *terminal;

    ydebug("terminal_create: cols=%u rows=%u pipe=%p", cols, rows, (void *)platform_input_pipe);

    terminal = calloc(1, sizeof(struct yetty_term_terminal));
    if (!terminal)
        return YETTY_ERR(yetty_term_terminal, "failed to allocate terminal");

    terminal->cols = cols;
    terminal->rows = rows;
    terminal->layer_count = 0;

    /* Create event loop */
    struct yetty_core_event_loop_result event_loop_res = yetty_core_event_loop_create(platform_input_pipe);
    if (!YETTY_IS_OK(event_loop_res)) {
        ydebug("terminal_create: failed to create event loop");
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create event loop");
    }
    terminal->event_loop = event_loop_res.value;
    ydebug("terminal_create: event_loop created at %p", (void *)terminal->event_loop);

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

    if (terminal->event_loop && terminal->event_loop->ops && terminal->event_loop->ops->destroy)
        terminal->event_loop->ops->destroy(terminal->event_loop);

    free(terminal);
}

struct yetty_core_void_result yetty_term_terminal_run(struct yetty_term_terminal *terminal)
{
    ydebug("terminal_run: Starting...");

    if (!terminal) {
        ydebug("terminal_run: terminal is null!");
        return YETTY_ERR(yetty_core_void, "terminal is null");
    }

    if (!terminal->event_loop) {
        ydebug("terminal_run: terminal has no event_loop!");
        return YETTY_ERR(yetty_core_void, "terminal has no event_loop");
    }

    if (!terminal->event_loop->ops) {
        ydebug("terminal_run: event_loop has no ops!");
        return YETTY_ERR(yetty_core_void, "event_loop has no ops");
    }

    if (!terminal->event_loop->ops->start) {
        ydebug("terminal_run: event_loop has no start op!");
        return YETTY_ERR(yetty_core_void, "event_loop has no start op");
    }

    ydebug("terminal_run: Calling event_loop start...");
    struct yetty_core_void_result res = terminal->event_loop->ops->start(terminal->event_loop);
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
    (void)terminal;
    (void)data;
    (void)len;
    /* TODO: route to appropriate layer via OSC parsing */
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
                    (terminal->layer_count - i - 1) * sizeof(*terminal->layers));
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
