/* libuv-event-loop.c - Event loop using libuv */

#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/types.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-poll-source.h>
#include <yetty/ytrace.h>
#include <uv.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define MAX_LISTENERS_PER_POLL 16
#define MAX_LISTENERS_PER_TIMER 16
#define MAX_LISTENERS_PER_TYPE 64
#define MAX_POLLS 256
#define MAX_TIMERS 64

struct poll_handle {
    union {
        uv_poll_t poll;
        uv_timer_t timer;  /* Windows: timer-based polling for non-socket handles */
    };
    int fd;
    int events;
    int active;
    int is_timer_poll;  /* 1 = using uv_timer (Windows HANDLEs), 0 = uv_poll (POSIX) */
#ifdef _WIN32
    void *win_handle;   /* raw HANDLE for PeekNamedPipe */
#endif
    struct yetty_core_event_listener *listeners[MAX_LISTENERS_PER_POLL];
    int listener_count;
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

    struct poll_handle polls[MAX_POLLS];
    int next_poll_id;

    struct timer_handle timers[MAX_TIMERS];
    int next_timer_id;

    struct yetty_platform_input_pipe *platform_input_pipe;
    uv_pipe_t pipe_handle;
    int pipe_poll_active;

    uv_async_t render_async;
    int render_pending;

    uv_signal_t sigint_handle;
    uv_signal_t sigterm_handle;
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
static struct yetty_core_poll_id_result libuv_create_poll(struct yetty_core_event_loop *self);
static struct yetty_core_poll_id_result libuv_create_pty_poll(
    struct yetty_core_event_loop *self, struct yetty_platform_pty_poll_source *source);
static struct yetty_core_void_result libuv_config_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id, int fd);
static struct yetty_core_void_result libuv_start_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id, int events);
static struct yetty_core_void_result libuv_stop_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id);
static struct yetty_core_void_result libuv_destroy_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id);
static struct yetty_core_void_result libuv_register_poll_listener(
    struct yetty_core_event_loop *self, yetty_core_poll_id id,
    struct yetty_core_event_listener *listener);
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
    .create_poll = libuv_create_poll,
    .create_pty_poll = libuv_create_pty_poll,
    .config_poll = libuv_config_poll,
    .start_poll = libuv_start_poll,
    .stop_poll = libuv_stop_poll,
    .destroy_poll = libuv_destroy_poll,
    .register_poll_listener = libuv_register_poll_listener,
    .create_timer = libuv_create_timer,
    .config_timer = libuv_config_timer,
    .start_timer = libuv_start_timer,
    .stop_timer = libuv_stop_timer,
    .destroy_timer = libuv_destroy_timer,
    .register_timer_listener = libuv_register_timer_listener,
    .request_render = libuv_request_render,
};

/* Callbacks */

static void on_signal(uv_signal_t *handle, int signum)
{
    struct libuv_event_loop *impl = handle->data;
    (void)signum;
    uv_stop(impl->loop);
}

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

static void on_pipe_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;
    /* We use a static buffer since events are processed synchronously */
    static char pipe_read_buf[4096];
    buf->base = pipe_read_buf;
    buf->len = sizeof(pipe_read_buf);
}

static void on_pipe_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct libuv_event_loop *impl = stream->data;

    if (nread <= 0)
        return;

    /* Process complete events from the buffer */
    size_t offset = 0;
    while (offset + sizeof(struct yetty_core_event) <= (size_t)nread) {
        struct yetty_core_event event;
        memcpy(&event, buf->base + offset, sizeof(event));
        libuv_dispatch(&impl->base, &event);
        offset += sizeof(struct yetty_core_event);
    }
}

#ifdef _WIN32
static void on_timer_poll(uv_timer_t *handle)
{
    struct poll_handle *ph = handle->data;
    DWORD available = 0;

    if (!ph->win_handle)
        return;

    /* Check if data is available without consuming it */
    if (PeekNamedPipe(ph->win_handle, NULL, 0, NULL, &available, NULL) && available > 0) {
        struct yetty_core_event event = {0};
        int i;
        event.type = YETTY_EVENT_POLL_READABLE;
        event.poll.fd = ph->fd;
        for (i = 0; i < ph->listener_count; i++)
            ph->listeners[i]->handler(ph->listeners[i], &event);
    }
}
#endif

static void on_poll(uv_poll_t *handle, int status, int events)
{
    struct poll_handle *ph = handle->data;
    struct yetty_core_event event = {0};
    int i;

    if (status < 0)
        return;

    if (events & UV_READABLE) {
        event.type = YETTY_EVENT_POLL_READABLE;
        event.poll.fd = ph->fd;
        for (i = 0; i < ph->listener_count; i++)
            ph->listeners[i]->handler(ph->listeners[i], &event);
    }

    if (events & UV_WRITABLE) {
        event.type = YETTY_EVENT_POLL_WRITABLE;
        event.poll.fd = ph->fd;
        for (i = 0; i < ph->listener_count; i++)
            ph->listeners[i]->handler(ph->listeners[i], &event);
    }
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

    uv_signal_stop(&impl->sigint_handle);
    uv_signal_stop(&impl->sigterm_handle);
    uv_close((uv_handle_t *)&impl->sigint_handle, NULL);
    uv_close((uv_handle_t *)&impl->sigterm_handle, NULL);
    uv_close((uv_handle_t *)&impl->render_async, NULL);

    if (impl->pipe_poll_active)
        uv_close((uv_handle_t *)&impl->pipe_handle, NULL);

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

    /* Find insert position (sorted by priority descending) */
    insert_pos = count;
    for (i = 0; i < count; i++) {
        if (impl->listeners[type][i].priority < priority) {
            insert_pos = i;
            break;
        }
    }

    /* Shift elements */
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
        if (listener->handler(listener, event))
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

static struct yetty_core_poll_id_result libuv_create_poll(struct yetty_core_event_loop *self)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    int id = impl->next_poll_id++;

    if (id >= MAX_POLLS)
        return YETTY_ERR(yetty_core_poll_id, "too many polls");

    memset(&impl->polls[id], 0, sizeof(impl->polls[id]));
    impl->polls[id].fd = -1;

    return YETTY_OK(yetty_core_poll_id, id);
}

static struct yetty_core_poll_id_result libuv_create_pty_poll(
    struct yetty_core_event_loop *self, struct yetty_platform_pty_poll_source *source)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct poll_handle *ph;
    int id, r;

    id = impl->next_poll_id++;
    if (id >= MAX_POLLS)
        return YETTY_ERR(yetty_core_poll_id, "too many polls");

    ph = &impl->polls[id];
    memset(ph, 0, sizeof(*ph));
    ph->fd = source->fd;

#ifdef _WIN32
    if (source->handle) {
        /* Windows: use timer-based polling — uv_poll only works with sockets,
           and uv_pipe_t consumes data (we need PeekNamedPipe without consuming) */
        r = uv_timer_init(impl->loop, &ph->timer);
        if (r != 0)
            return YETTY_ERR(yetty_core_poll_id, "uv_timer_init failed");
        ph->timer.data = ph;
        ph->win_handle = source->handle;
        ph->is_timer_poll = 1;
        return YETTY_OK(yetty_core_poll_id, id);
    }
#endif

    if (source->fd < 0)
        return YETTY_ERR(yetty_core_poll_id, "invalid pty poll source");

    r = uv_poll_init(impl->loop, &ph->poll, ph->fd);
    if (r != 0)
        return YETTY_ERR(yetty_core_poll_id, "uv_poll_init failed");

    ph->poll.data = ph;
    return YETTY_OK(yetty_core_poll_id, id);
}

static struct yetty_core_void_result libuv_config_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id, int fd)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct poll_handle *ph;
    int r;

    if (id < 0 || id >= MAX_POLLS)
        return YETTY_ERR(yetty_core_void, "invalid poll id");

    ph = &impl->polls[id];
    ph->fd = fd;
    r = uv_poll_init(impl->loop, &ph->poll, fd);
    if (r != 0)
        return YETTY_ERR(yetty_core_void, "uv_poll_init failed");

    ph->poll.data = ph;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_start_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id, int events)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct poll_handle *ph;
    int uv_events = 0;
    int r;

    if (id < 0 || id >= MAX_POLLS)
        return YETTY_ERR(yetty_core_void, "invalid poll id");

    ph = &impl->polls[id];

    if (events & YETTY_CORE_POLL_READABLE)
        uv_events |= UV_READABLE;
    if (events & YETTY_CORE_POLL_WRITABLE)
        uv_events |= UV_WRITABLE;

    ph->events = uv_events;

    if (ph->is_timer_poll) {
        r = uv_timer_start(&ph->timer, on_timer_poll, 0, 1); /* 1ms repeat */
    } else {
        r = uv_poll_start(&ph->poll, uv_events, on_poll);
    }
    if (r != 0)
        return YETTY_ERR(yetty_core_void, "poll start failed");

    ph->active = 1;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_stop_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

    if (id < 0 || id >= MAX_POLLS)
        return YETTY_ERR(yetty_core_void, "invalid poll id");

    if (impl->polls[id].is_timer_poll)
        uv_timer_stop(&impl->polls[id].timer);
    else
        uv_poll_stop(&impl->polls[id].poll);
    impl->polls[id].active = 0;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_destroy_poll(
    struct yetty_core_event_loop *self, yetty_core_poll_id id)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);

    if (id < 0 || id >= MAX_POLLS)
        return YETTY_ERR(yetty_core_void, "invalid poll id");

    uv_poll_stop(&impl->polls[id].poll);
    uv_close((uv_handle_t *)&impl->polls[id].poll, NULL);
    impl->polls[id].active = 0;
    return YETTY_OK_VOID();
}

static struct yetty_core_void_result libuv_register_poll_listener(
    struct yetty_core_event_loop *self, yetty_core_poll_id id,
    struct yetty_core_event_listener *listener)
{
    struct libuv_event_loop *impl = container_of(self, struct libuv_event_loop, base);
    struct poll_handle *ph;

    if (id < 0 || id >= MAX_POLLS || !listener)
        return YETTY_ERR(yetty_core_void, "invalid poll id or listener");

    ph = &impl->polls[id];
    if (ph->listener_count >= MAX_LISTENERS_PER_POLL)
        return YETTY_ERR(yetty_core_void, "too many poll listeners");

    ph->listeners[ph->listener_count++] = listener;
    return YETTY_OK_VOID();
}

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
    impl->next_poll_id = 0;
    impl->next_timer_id = 0;

    /* Render async */
    impl->render_async.data = impl;
    uv_async_init(impl->loop, &impl->render_async, on_render_async);

    /* Signal handlers */
    impl->sigint_handle.data = impl;
    impl->sigterm_handle.data = impl;
    uv_signal_init(impl->loop, &impl->sigint_handle);
    uv_signal_init(impl->loop, &impl->sigterm_handle);
    uv_signal_start(&impl->sigint_handle, on_signal, SIGINT);
    uv_signal_start(&impl->sigterm_handle, on_signal, SIGTERM);

    /* Pipe reading via uv_pipe_t (works on both POSIX and Windows) */
    if (pipe) {
        struct yetty_core_int_result fd_res = pipe->ops->read_fd(pipe);
        if (YETTY_IS_OK(fd_res) && fd_res.value >= 0) {
            uv_pipe_init(impl->loop, &impl->pipe_handle, 0);
            impl->pipe_handle.data = impl;
            uv_pipe_open(&impl->pipe_handle, fd_res.value);
            uv_read_start((uv_stream_t *)&impl->pipe_handle, on_pipe_alloc, on_pipe_read);
            impl->pipe_poll_active = 1;
        }
    }

    return YETTY_OK(yetty_core_event_loop, &impl->base);
}
