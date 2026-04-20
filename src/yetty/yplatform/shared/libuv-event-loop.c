/* libuv-event-loop.c - Event loop using libuv with uv_pipe_t for all pipes
 *
 * Works on both Unix and Windows:
 * - Input pipe (bridge): uv_pipe_t reads structured events from main thread
 * - PTY pipe: uv_pipe_t reads raw PTY output, feeds to pty_reader
 * - Both use IOCP on Windows, epoll/kqueue on Unix — zero CPU when idle
 */

#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/types.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-pipe-source.h>
#include <yetty/ytrace.h>
#include <uv.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#endif

#define MAX_LISTENERS_PER_TIMER 16
#define MAX_LISTENERS_PER_TYPE 64
#define MAX_PTY_PIPES 16
#define MAX_TIMERS 64

struct pty_pipe_handle {
    uv_pipe_t pipe;
    yetty_pipe_alloc_cb alloc_cb;
    yetty_pipe_read_cb read_cb;
    void *cb_ctx;
    int active;
};

struct timer_handle {
    uv_timer_t timer;
    int id;
    int timeout_ms;
    int active;
    struct yetty_core_event_listener *listeners[MAX_LISTENERS_PER_TIMER];
    int listener_count;
};

struct prioritized_listener {
    struct yetty_core_event_listener *listener;
    int priority;
};

struct libuv_event_loop {
    struct yetty_core_event_loop base;
    uv_loop_t *loop;

    struct prioritized_listener listeners[YETTY_EVENT_COUNT][MAX_LISTENERS_PER_TYPE];
    int listener_counts[YETTY_EVENT_COUNT];

    struct pty_pipe_handle pty_pipes[MAX_PTY_PIPES];
    int next_pty_pipe_id;

    struct timer_handle timers[MAX_TIMERS];
    int next_timer_id;

    struct yetty_platform_input_pipe *platform_input_pipe;
    uv_pipe_t input_pipe;
    int input_pipe_active;

    uv_async_t render_async;
    int render_pending;

#ifndef _WIN32
    uv_signal_t sigint_handle;
    uv_signal_t sigterm_handle;
#endif
};

/* Forward declarations */
static void libuv_destroy(struct yetty_core_event_loop *self);
static struct yetty_core_void_result libuv_start(struct yetty_core_event_loop *self);
static struct yetty_core_void_result libuv_stop(struct yetty_core_event_loop *self);
static struct yetty_core_void_result libuv_register_listener(
    struct yetty_core_event_loop *self, enum yetty_core_event_type type,
    struct yetty_core_event_listener *listener, int priority);
static struct yetty_core_void_result libuv_deregister_listener(
    struct yetty_core_event_loop *self, enum yetty_core_event_type type,
    struct yetty_core_event_listener *listener);
static struct yetty_core_int_result libuv_dispatch(
    struct yetty_core_event_loop *self, const struct yetty_core_event *event);
static struct yetty_core_void_result libuv_broadcast(
    struct yetty_core_event_loop *self, const struct yetty_core_event *event);
static struct yetty_core_pipe_id_result libuv_register_pty_pipe(
    struct yetty_core_event_loop *self,
    struct yetty_platform_pty_pipe_source *source,
    yetty_pipe_alloc_cb alloc_cb,
    yetty_pipe_read_cb read_cb,
    void *cb_ctx);
static struct yetty_core_void_result libuv_unregister_pty_pipe(
    struct yetty_core_event_loop *self, yetty_core_pipe_id id);
static struct yetty_core_timer_id_result libuv_create_timer(struct yetty_core_event_loop *self);
static struct yetty_core_void_result libuv_config_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id, int timeout_ms);
static struct yetty_core_void_result libuv_start_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id);
static struct yetty_core_void_result libuv_stop_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id);
static struct yetty_core_void_result libuv_destroy_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id);
static struct yetty_core_void_result libuv_register_timer_listener(
    struct yetty_core_event_loop *self, yetty_core_timer_id id,
    struct yetty_core_event_listener *listener);
static void libuv_request_render(struct yetty_core_event_loop *self);

static const struct yetty_core_event_loop_ops libuv_ops = {
    .destroy = libuv_destroy,
    .start = libuv_start,
    .stop = libuv_stop,
    .register_listener = libuv_register_listener,
    .deregister_listener = libuv_deregister_listener,
    .dispatch = libuv_dispatch,
    .broadcast = libuv_broadcast,
    .register_pty_pipe = libuv_register_pty_pipe,
    .unregister_pty_pipe = libuv_unregister_pty_pipe,
    .create_timer = libuv_create_timer,
    .config_timer = libuv_config_timer,
    .start_timer = libuv_start_timer,
    .stop_timer = libuv_stop_timer,
    .destroy_timer = libuv_destroy_timer,
    .register_timer_listener = libuv_register_timer_listener,
    .request_render = libuv_request_render,
};

/* Callbacks */

#ifndef _WIN32
static void on_signal(uv_signal_t *handle, int signum)
{
    struct libuv_event_loop *impl = handle->data;
    (void)signum;
    uv_stop(impl->loop);
}
#endif

static void on_render_async(uv_async_t *handle)
{
    struct libuv_event_loop *impl = handle->data;
    struct yetty_core_event event = {0};

    ydebug("on_render_async: render_pending=%d", impl->render_pending);
    if (impl->render_pending) {
        impl->render_pending = 0;
        event.type = YETTY_EVENT_RENDER;
        libuv_dispatch(&impl->base, &event);
    }
}

/* Input pipe (bridge) callbacks — uv_pipe_t reads structured events */

static void on_input_pipe_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;
    static char input_pipe_buf[4096];
    buf->base = input_pipe_buf;
    buf->len = sizeof(input_pipe_buf);
}

static void on_input_pipe_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct libuv_event_loop *impl = stream->data;

    if (nread <= 0)
        return;

    size_t offset = 0;
    while (offset + sizeof(struct yetty_core_event) <= (size_t)nread) {
        struct yetty_core_event event;
        memcpy(&event, buf->base + offset, sizeof(event));
        libuv_dispatch(&impl->base, &event);
        offset += sizeof(struct yetty_core_event);
    }
}

/* PTY pipe callbacks — delegate to caller-provided callbacks */

static void on_pty_pipe_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    struct pty_pipe_handle *ph = handle->data;
    char *base = NULL;
    size_t len = 0;
    ph->alloc_cb(ph->cb_ctx, suggested_size, &base, &len);
    buf->base = base;
    buf->len = len;
}

static void on_pty_pipe_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct pty_pipe_handle *ph = stream->data;
    ph->read_cb(ph->cb_ctx, buf->base, (long)nread);
}

static void on_timer(uv_timer_t *handle)
{
    struct timer_handle *th = handle->data;
    struct yetty_core_event event = {0};
    int i;

    event.type = YETTY_EVENT_TIMER;
    event.timer.timer_id = th->id;

    for (i = 0; i < th->listener_count; i++)
        th->listeners[i]->handler(th->listeners[i], &event);
}

/* Implementation */

static void libuv_destroy(struct yetty_core_event_loop *self)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

#ifndef _WIN32
    uv_signal_stop(&impl->sigint_handle);
    uv_signal_stop(&impl->sigterm_handle);
    uv_close((uv_handle_t *)&impl->sigint_handle, NULL);
    uv_close((uv_handle_t *)&impl->sigterm_handle, NULL);
#endif
    uv_close((uv_handle_t *)&impl->render_async, NULL);

    if (impl->input_pipe_active)
        uv_close((uv_handle_t *)&impl->input_pipe, NULL);

    free(impl);
}

static struct yetty_core_void_result libuv_start(struct yetty_core_event_loop *self)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int r = uv_run(impl->loop, UV_RUN_DEFAULT);

    if (r < 0)
        return YETTY_ERR(yetty_core_void, "uv_run failed");

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_stop(struct yetty_core_event_loop *self)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    uv_stop(impl->loop);
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_register_listener(
    struct yetty_core_event_loop *self, enum yetty_core_event_type type,
    struct yetty_core_event_listener *listener, int priority)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int count, i, insert_pos;

    if (!listener || type >= YETTY_EVENT_COUNT)
        return YETTY_ERR(yetty_core_void, "invalid listener or type");

    count = impl->listener_counts[type];
    if (count >= MAX_LISTENERS_PER_TYPE)
        return YETTY_ERR(yetty_core_void, "too many listeners");

    insert_pos = count;
    for (i = 0; i < count; i++) {
        if (impl->listeners[type][i].priority < priority) {
            insert_pos = i;
            break;
        }
    }

    for (i = count; i > insert_pos; i--)
        impl->listeners[type][i] = impl->listeners[type][i - 1];

    impl->listeners[type][insert_pos].listener = listener;
    impl->listeners[type][insert_pos].priority = priority;
    impl->listener_counts[type]++;

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_deregister_listener(
    struct yetty_core_event_loop *self, enum yetty_core_event_type type,
    struct yetty_core_event_listener *listener)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int count, i, j;

    if (type >= YETTY_EVENT_COUNT)
        return YETTY_OK_VOID();

    count = impl->listener_counts[type];
    for (i = 0; i < count; i++) {
        if (impl->listeners[type][i].listener == listener) {
            for (j = i; j < count - 1; j++)
                impl->listeners[type][j] = impl->listeners[type][j + 1];
            impl->listener_counts[type]--;
            break;
        }
    }

    return YETTY_OK_VOID();
}

static struct yetty_core_int_result libuv_dispatch(
    struct yetty_core_event_loop *self, const struct yetty_core_event *event)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int count, i;

    if (event->type >= YETTY_EVENT_COUNT)
        return YETTY_OK(yetty_core_int, 0);

    count = impl->listener_counts[event->type];
    for (i = 0; i < count; i++) {
        struct yetty_core_event_listener *listener = impl->listeners[event->type][i].listener;
        struct yetty_core_int_result res = listener->handler(listener, event);
        if (YETTY_IS_ERR(res))
            return res;
        if (res.value)
            return YETTY_OK(yetty_core_int, 1);
    }

    return YETTY_OK(yetty_core_int, 0);
}

static struct yetty_core_void_result libuv_broadcast(
    struct yetty_core_event_loop *self, const struct yetty_core_event *event)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int t, count, i;

    for (t = 0; t < YETTY_EVENT_COUNT; t++) {
        count = impl->listener_counts[t];
        for (i = 0; i < count; i++) {
            struct yetty_core_event_listener *listener = impl->listeners[t][i].listener;
            listener->handler(listener, event);
        }
    }

    return YETTY_OK_VOID();
}

/* PTY pipe — uv_pipe_t with uv_read_start, data feeds to pty_reader */

static struct yetty_core_pipe_id_result libuv_register_pty_pipe(
    struct yetty_core_event_loop *self,
    struct yetty_platform_pty_pipe_source *source,
    yetty_pipe_alloc_cb alloc_cb,
    yetty_pipe_read_cb read_cb,
    void *cb_ctx)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct pty_pipe_handle *ph;
    int id, r;

    if (!source || !alloc_cb || !read_cb)
        return YETTY_ERR(yetty_core_pipe_id, "invalid source or callbacks");

    id = impl->next_pty_pipe_id++;
    if (id >= MAX_PTY_PIPES)
        return YETTY_ERR(yetty_core_pipe_id, "too many pty pipes");

    ph = &impl->pty_pipes[id];
    memset(ph, 0, sizeof(*ph));
    ph->alloc_cb = alloc_cb;
    ph->read_cb = read_cb;
    ph->cb_ctx = cb_ctx;

    r = uv_pipe_init(impl->loop, &ph->pipe, 0);
    if (r != 0)
        return YETTY_ERR(yetty_core_pipe_id, "uv_pipe_init failed");

    r = uv_pipe_open(&ph->pipe, (uv_file)source->abstract);
    if (r != 0)
        return YETTY_ERR(yetty_core_pipe_id, "uv_pipe_open failed");

    ph->pipe.data = ph;

    r = uv_read_start((uv_stream_t *)&ph->pipe, on_pty_pipe_alloc, on_pty_pipe_read);
    if (r != 0)
        return YETTY_ERR(yetty_core_pipe_id, "uv_read_start failed");

    ph->active = 1;
    return YETTY_OK(yetty_core_pipe_id, id);
}

static struct yetty_core_void_result libuv_unregister_pty_pipe(
    struct yetty_core_event_loop *self, yetty_core_pipe_id id)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

    if (id < 0 || id >= MAX_PTY_PIPES)
        return YETTY_ERR(yetty_core_void, "invalid pipe id");

    if (impl->pty_pipes[id].active) {
        uv_read_stop((uv_stream_t *)&impl->pty_pipes[id].pipe);
        uv_close((uv_handle_t *)&impl->pty_pipes[id].pipe, NULL);
        impl->pty_pipes[id].active = 0;
    }

    return YETTY_OK_VOID();
}

/* Timers */

static struct yetty_core_timer_id_result libuv_create_timer(struct yetty_core_event_loop *self)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int id = impl->next_timer_id++;
    struct timer_handle *th;

    if (id >= MAX_TIMERS)
        return YETTY_ERR(yetty_core_timer_id, "too many timers");

    th = &impl->timers[id];
    memset(th, 0, sizeof(*th));
    th->id = id;
    uv_timer_init(impl->loop, &th->timer);
    th->timer.data = th;

    return YETTY_OK(yetty_core_timer_id, id);
}

static struct yetty_core_void_result libuv_config_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id, int timeout_ms)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

    if (id < 0 || id >= MAX_TIMERS)
        return YETTY_ERR(yetty_core_void, "invalid timer id");

    impl->timers[id].timeout_ms = timeout_ms;

    if (impl->timers[id].active)
        uv_timer_start(&impl->timers[id].timer, on_timer, timeout_ms, timeout_ms);

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_start_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct timer_handle *th;

    if (id < 0 || id >= MAX_TIMERS)
        return YETTY_ERR(yetty_core_void, "invalid timer id");

    th = &impl->timers[id];
    uv_timer_start(&th->timer, on_timer, th->timeout_ms, th->timeout_ms);
    th->active = 1;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_stop_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

    if (id < 0 || id >= MAX_TIMERS)
        return YETTY_ERR(yetty_core_void, "invalid timer id");

    uv_timer_stop(&impl->timers[id].timer);
    impl->timers[id].active = 0;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_destroy_timer(
    struct yetty_core_event_loop *self, yetty_core_timer_id id)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

    if (id < 0 || id >= MAX_TIMERS)
        return YETTY_ERR(yetty_core_void, "invalid timer id");

    uv_timer_stop(&impl->timers[id].timer);
    uv_close((uv_handle_t *)&impl->timers[id].timer, NULL);
    impl->timers[id].active = 0;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_register_timer_listener(
    struct yetty_core_event_loop *self, yetty_core_timer_id id,
    struct yetty_core_event_listener *listener)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct timer_handle *th;

    if (id < 0 || id >= MAX_TIMERS || !listener)
        return YETTY_ERR(yetty_core_void, "invalid timer id or listener");

    th = &impl->timers[id];
    if (th->listener_count >= MAX_LISTENERS_PER_TIMER)
        return YETTY_ERR(yetty_core_void, "too many timer listeners");

    th->listeners[th->listener_count++] = listener;
    return YETTY_OK_VOID();
}

static void libuv_request_render(struct yetty_core_event_loop *self)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    ydebug("libuv_request_render: setting render_pending=1");
    impl->render_pending = 1;
    uv_async_send(&impl->render_async);
}

struct yetty_core_event_loop_result yetty_core_event_loop_create(
    struct yetty_platform_input_pipe *pipe)
{
    struct libuv_event_loop *impl;

    impl = malloc(sizeof(struct libuv_event_loop));
    if (!impl)
        return YETTY_ERR(yetty_core_event_loop, "failed to allocate event loop");

    memset(impl, 0, sizeof(*impl));
    impl->base.ops = &libuv_ops;
    impl->loop = uv_default_loop();
    impl->platform_input_pipe = pipe;

    /* Render async */
    impl->render_async.data = impl;
    uv_async_init(impl->loop, &impl->render_async, on_render_async);

#ifndef _WIN32
    /* Signal handlers (Unix only) */
    impl->sigint_handle.data = impl;
    impl->sigterm_handle.data = impl;
    uv_signal_init(impl->loop, &impl->sigint_handle);
    uv_signal_init(impl->loop, &impl->sigterm_handle);
    uv_signal_start(&impl->sigint_handle, on_signal, SIGINT);
    uv_signal_start(&impl->sigterm_handle, on_signal, SIGTERM);
#endif

    /* Input pipe via uv_pipe_t */
    if (pipe) {
        struct yetty_core_int_result fd_res = pipe->ops->read_fd(pipe);
        if (YETTY_IS_OK(fd_res) && fd_res.value >= 0) {
            uv_pipe_init(impl->loop, &impl->input_pipe, 0);
            impl->input_pipe.data = impl;
            uv_pipe_open(&impl->input_pipe, fd_res.value);
            uv_read_start((uv_stream_t *)&impl->input_pipe,
                          on_input_pipe_alloc, on_input_pipe_read);
            impl->input_pipe_active = 1;
        }
    }

    return YETTY_OK(yetty_core_event_loop, &impl->base);
}
