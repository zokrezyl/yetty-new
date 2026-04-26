/*
 * ymgui_event_loop.c — libuv-backed event loop for ymgui frontend.
 *
 * I/O model:
 *
 *   stdin  -> uv_poll (READABLE) -> read(2) into temp buf
 *               -> yetty_yface_feed_bytes(stream)
 *                    -> on_osc(code, payload, len) -> typed dispatch
 *                    -> on_raw(bytes, n)            -> app's raw cb
 *               -> set frame_pending
 *   stdout -> uv_poll (WRITABLE) [activated only when out queue has tail]
 *
 * Frame coalescing: a uv_check_t runs once per iteration after I/O. If
 * frame_pending is set (any input fired or request_frame was called) it
 * fires the user's frame_cb and clears the flag.
 *
 * App extensions: small fixed-size tables of uv_poll_t / uv_timer_t /
 * pending posted tasks. Sized to typical ImGui-app needs; can grow if
 * required, but the goal is to stay simple.
 */

#include "ymgui_event_loop.h"
#include "ymgui_encode.h"

#include <yetty/yface/yface.h>
#include <yetty/ymgui/wire.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <uv.h>

#define YMGUI_LOOP_FD_SLOTS    16
#define YMGUI_LOOP_TIMER_SLOTS 16
#define YMGUI_LOOP_READ_BUF    8192

/*=============================================================================
 * App extension slots
 *===========================================================================*/

struct loop_fd_slot {
    uv_poll_t    poll;
    int          id;
    int          fd;
    int          events_mask;
    ymgui_fd_cb  cb;
    void        *cb_user;
    int          active;
    struct ymgui_event_loop *loop;
};

struct loop_timer_slot {
    uv_timer_t      timer;
    int             id;
    int             timeout_ms;
    int             repeat_ms;
    ymgui_timer_cb  cb;
    void           *cb_user;
    int             active;
    struct ymgui_event_loop *loop;
};

/* Single-linked queue of (fn, arg) callbacks scheduled via post(). Drained
 * by the post_async handler. */
struct post_node {
    ymgui_task_cb     cb;
    void             *cb_user;
    struct post_node *next;
};

struct ymgui_event_loop {
    uv_loop_t       loop;
    int             owns_loop;        /* always 1 — we never embed an external loop yet */

    int             in_fd;
    int             out_fd;
    void           *user;

    /* I/O handles */
    uv_poll_t       in_poll;
    int             in_poll_active;
    uv_poll_t       out_poll;
    int             out_poll_active;

    /* Stop / wake */
    uv_async_t      stop_async;
    uv_async_t      frame_async;
    uv_async_t      post_async;
    uv_check_t      frame_check;

    /* Stream decoder (owns a yface configured with set_handlers). */
    struct yetty_yface *yface;

    /* Input callbacks */
    ymgui_mouse_pos_cb     on_pos;
    ymgui_mouse_button_cb  on_btn;
    ymgui_mouse_wheel_cb   on_wheel;
    ymgui_resize_cb        on_resize;
    ymgui_raw_cb           on_raw;
    ymgui_frame_cb         on_frame;

    /* Frame coalescing */
    int             frame_pending;

    /* App extensions */
    struct loop_fd_slot     fd_slots   [YMGUI_LOOP_FD_SLOTS];
    struct loop_timer_slot  timer_slots[YMGUI_LOOP_TIMER_SLOTS];
    int                     next_fd_id;
    int                     next_timer_id;

    /* Posted tasks (cross-thread) */
    uv_mutex_t        post_mutex;
    struct post_node *post_head;
    struct post_node *post_tail;
};

/*=============================================================================
 * yface dispatch
 *===========================================================================*/

static void on_yface_osc(void *user, int osc_code,
                         const uint8_t *payload, size_t len)
{
    struct ymgui_event_loop *L = user;

    switch (osc_code) {
    case YMGUI_OSC_SC_MOUSE: {
        if (len < sizeof(struct ymgui_wire_input_mouse)) return;
        const struct ymgui_wire_input_mouse *m =
            (const struct ymgui_wire_input_mouse *)payload;
        if (m->magic != YMGUI_WIRE_MAGIC_INPUT_MOUSE) return;
        switch (m->kind) {
        case YMGUI_INPUT_MOUSE_POS:
            if (L->on_pos)
                L->on_pos(L->user, m->x, m->y, m->buttons_held);
            break;
        case YMGUI_INPUT_MOUSE_BUTTON:
            if (L->on_btn)
                L->on_btn(L->user, m->button, m->pressed, m->x, m->y);
            break;
        case YMGUI_INPUT_MOUSE_WHEEL:
            if (L->on_wheel)
                L->on_wheel(L->user, m->wheel_dy, m->x, m->y);
            break;
        }
        L->frame_pending = 1;
        break;
    }
    case YMGUI_OSC_SC_RESIZE: {
        if (len < sizeof(struct ymgui_wire_input_resize)) return;
        const struct ymgui_wire_input_resize *r =
            (const struct ymgui_wire_input_resize *)payload;
        if (r->magic != YMGUI_WIRE_MAGIC_INPUT_RESIZE) return;
        if (L->on_resize)
            L->on_resize(L->user, r->width, r->height);
        L->frame_pending = 1;
        break;
    }
    default:
        /* Unknown OSC code — silently ignore so apps tolerate vendor
         * messages they don't recognise. */
        break;
    }
}

static void on_yface_raw(void *user, const char *bytes, size_t n)
{
    struct ymgui_event_loop *L = user;
    if (L->on_raw)
        L->on_raw(L->user, bytes, n);
    /* Raw bytes (keyboard, CSI) usually do change app state — request a
     * frame so the app can react. */
    L->frame_pending = 1;
}

/*=============================================================================
 * stdin / stdout polling
 *===========================================================================*/

static void on_in_readable(uv_poll_t *handle, int status, int events)
{
    struct ymgui_event_loop *L = handle->data;
    if (status < 0 || !(events & UV_READABLE)) return;

    char buf[YMGUI_LOOP_READ_BUF];
    for (;;) {
        ssize_t n = read(L->in_fd, buf, sizeof(buf));
        if (n > 0) {
            yetty_yface_feed_bytes(L->yface, buf, (size_t)n);
            continue;
        }
        if (n == 0) break;            /* EOF */
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        break;
    }
}

static void on_out_writable(uv_poll_t *handle, int status, int events)
{
    struct ymgui_event_loop *L = handle->data;
    if (status < 0 || !(events & UV_WRITABLE)) return;

    /* Drain whatever the encoder queued up. If the queue is empty, stop
     * watching for writability (otherwise we'd spin on POLLOUT). */
    int rc = ymgui_pending_flush(L->out_fd);
    if (rc != 1) {
        uv_poll_stop(&L->out_poll);
        L->out_poll_active = 0;
    }
}

/*=============================================================================
 * Frame coalescer + cross-thread asyncs
 *===========================================================================*/

static void on_frame_check(uv_check_t *handle)
{
    struct ymgui_event_loop *L = handle->data;

    /* If we have queued bytes and stdout isn't yet being watched, start
     * watching now. */
    if (ymgui_pending_active() && !L->out_poll_active) {
        uv_poll_start(&L->out_poll, UV_WRITABLE, on_out_writable);
        L->out_poll_active = 1;
    }

    if (!L->frame_pending) return;
    L->frame_pending = 0;
    if (L->on_frame) L->on_frame(L->user);
}

static void on_stop_async(uv_async_t *handle)
{
    struct ymgui_event_loop *L = handle->data;
    uv_stop(&L->loop);
}

static void on_frame_async(uv_async_t *handle)
{
    struct ymgui_event_loop *L = handle->data;
    L->frame_pending = 1;
}

static void on_post_async(uv_async_t *handle)
{
    struct ymgui_event_loop *L = handle->data;

    struct post_node *head;
    uv_mutex_lock(&L->post_mutex);
    head        = L->post_head;
    L->post_head = NULL;
    L->post_tail = NULL;
    uv_mutex_unlock(&L->post_mutex);

    while (head) {
        struct post_node *next = head->next;
        if (head->cb) head->cb(head->cb_user);
        free(head);
        head = next;
    }
}

/*=============================================================================
 * Lifecycle
 *===========================================================================*/

struct ymgui_event_loop *ymgui_event_loop_create(
    const struct ymgui_event_loop_config *cfg)
{
    struct ymgui_event_loop *L = calloc(1, sizeof(*L));
    if (!L) return NULL;

    L->in_fd  = (cfg && cfg->in_fd  >= 0) ? cfg->in_fd  : STDIN_FILENO;
    L->out_fd = (cfg && cfg->out_fd >= 0) ? cfg->out_fd : STDOUT_FILENO;
    L->user   = cfg ? cfg->user : NULL;
    L->next_fd_id    = 1;
    L->next_timer_id = 1;

    if (uv_loop_init(&L->loop) != 0) goto fail;
    L->owns_loop = 1;

    /* Stream decoder. */
    {
        struct yetty_yface_ptr_result yr = yetty_yface_create();
        if (!yr.ok) goto fail;
        L->yface = yr.value;
        yetty_yface_set_handlers(L->yface, on_yface_osc, on_yface_raw, L);
    }

    /* uv_poll on stdin. uv_poll_init_socket would set non-blocking; for
     * a regular fd uv_poll_init does the right thing. */
    if (uv_poll_init(&L->loop, &L->in_poll, L->in_fd) != 0) goto fail;
    L->in_poll.data = L;
    if (uv_poll_start(&L->in_poll, UV_READABLE, on_in_readable) != 0) goto fail;
    L->in_poll_active = 1;

    if (uv_poll_init(&L->loop, &L->out_poll, L->out_fd) != 0) goto fail;
    L->out_poll.data = L;
    /* not started — we only watch when we have a queued tail */

    if (uv_async_init(&L->loop, &L->stop_async, on_stop_async) != 0) goto fail;
    L->stop_async.data = L;
    if (uv_async_init(&L->loop, &L->frame_async, on_frame_async) != 0) goto fail;
    L->frame_async.data = L;
    if (uv_async_init(&L->loop, &L->post_async, on_post_async) != 0) goto fail;
    L->post_async.data = L;

    if (uv_check_init(&L->loop, &L->frame_check) != 0) goto fail;
    L->frame_check.data = L;
    if (uv_check_start(&L->frame_check, on_frame_check) != 0) goto fail;

    if (uv_mutex_init(&L->post_mutex) != 0) goto fail;

    return L;

fail:
    ymgui_event_loop_destroy(L);
    return NULL;
}

static void on_handle_close(uv_handle_t *h) { (void)h; }

void ymgui_event_loop_destroy(struct ymgui_event_loop *L)
{
    if (!L) return;

    /* Stop + close all live handles before tearing down the loop. */
    if (L->in_poll_active)  uv_poll_stop(&L->in_poll);
    if (L->out_poll_active) uv_poll_stop(&L->out_poll);
    uv_check_stop(&L->frame_check);

    for (int i = 0; i < YMGUI_LOOP_FD_SLOTS; i++) {
        if (L->fd_slots[i].active) {
            uv_poll_stop(&L->fd_slots[i].poll);
            uv_close((uv_handle_t *)&L->fd_slots[i].poll, on_handle_close);
            L->fd_slots[i].active = 0;
        }
    }
    for (int i = 0; i < YMGUI_LOOP_TIMER_SLOTS; i++) {
        if (L->timer_slots[i].active) {
            uv_timer_stop(&L->timer_slots[i].timer);
            uv_close((uv_handle_t *)&L->timer_slots[i].timer, on_handle_close);
            L->timer_slots[i].active = 0;
        }
    }

    if (uv_is_active((uv_handle_t *)&L->in_poll))
        uv_close((uv_handle_t *)&L->in_poll, on_handle_close);
    if (uv_is_active((uv_handle_t *)&L->out_poll))
        uv_close((uv_handle_t *)&L->out_poll, on_handle_close);
    uv_close((uv_handle_t *)&L->stop_async,  on_handle_close);
    uv_close((uv_handle_t *)&L->frame_async, on_handle_close);
    uv_close((uv_handle_t *)&L->post_async,  on_handle_close);
    uv_close((uv_handle_t *)&L->frame_check, on_handle_close);

    /* Drain pending closes. */
    if (L->owns_loop)
        uv_run(&L->loop, UV_RUN_NOWAIT);

    uv_mutex_destroy(&L->post_mutex);

    if (L->owns_loop) uv_loop_close(&L->loop);

    if (L->yface) yetty_yface_destroy(L->yface);

    /* Drain post queue (in case any tasks were posted after destroy). */
    while (L->post_head) {
        struct post_node *n = L->post_head;
        L->post_head = n->next;
        free(n);
    }

    free(L);
}

/*=============================================================================
 * Callback wiring
 *===========================================================================*/
void ymgui_event_loop_set_user           (struct ymgui_event_loop *L, void *u)               { L->user = u; }
void ymgui_event_loop_set_mouse_pos_cb   (struct ymgui_event_loop *L, ymgui_mouse_pos_cb c)    { L->on_pos    = c; }
void ymgui_event_loop_set_mouse_button_cb(struct ymgui_event_loop *L, ymgui_mouse_button_cb c) { L->on_btn    = c; }
void ymgui_event_loop_set_mouse_wheel_cb (struct ymgui_event_loop *L, ymgui_mouse_wheel_cb c)  { L->on_wheel  = c; }
void ymgui_event_loop_set_resize_cb      (struct ymgui_event_loop *L, ymgui_resize_cb c)       { L->on_resize = c; }
void ymgui_event_loop_set_raw_cb         (struct ymgui_event_loop *L, ymgui_raw_cb c)          { L->on_raw    = c; }
void ymgui_event_loop_set_frame_cb       (struct ymgui_event_loop *L, ymgui_frame_cb c)        { L->on_frame  = c; }

/*=============================================================================
 * Run
 *===========================================================================*/

int ymgui_event_loop_run(struct ymgui_event_loop *L)
{
    return uv_run(&L->loop, UV_RUN_DEFAULT);
}

int ymgui_event_loop_poll(struct ymgui_event_loop *L, int wait)
{
    return uv_run(&L->loop, wait ? UV_RUN_ONCE : UV_RUN_NOWAIT);
}

void ymgui_event_loop_stop(struct ymgui_event_loop *L)
{
    uv_async_send(&L->stop_async);
}

void ymgui_event_loop_request_frame(struct ymgui_event_loop *L)
{
    uv_async_send(&L->frame_async);
}

/*=============================================================================
 * App extensions: fds
 *===========================================================================*/

static struct loop_fd_slot *find_fd_slot(struct ymgui_event_loop *L, int id)
{
    for (int i = 0; i < YMGUI_LOOP_FD_SLOTS; i++)
        if (L->fd_slots[i].active && L->fd_slots[i].id == id)
            return &L->fd_slots[i];
    return NULL;
}

static void on_app_fd(uv_poll_t *handle, int status, int events)
{
    struct loop_fd_slot *slot = handle->data;
    if (status < 0 || !slot->cb) return;

    int mask = 0;
    if (events & UV_READABLE) mask |= YMGUI_FD_READABLE;
    if (events & UV_WRITABLE) mask |= YMGUI_FD_WRITABLE;
    slot->cb(slot->cb_user, slot->fd, mask);
}

int ymgui_event_loop_add_fd(struct ymgui_event_loop *L,
                            int fd, int events_mask,
                            ymgui_fd_cb cb, void *cb_user)
{
    if (!L || fd < 0 || !cb) return 0;

    int idx = -1;
    for (int i = 0; i < YMGUI_LOOP_FD_SLOTS; i++) {
        if (!L->fd_slots[i].active) { idx = i; break; }
    }
    if (idx < 0) return 0;

    struct loop_fd_slot *s = &L->fd_slots[idx];
    if (uv_poll_init(&L->loop, &s->poll, fd) != 0) return 0;
    s->poll.data = s;
    s->loop      = L;
    s->id        = L->next_fd_id++;
    s->fd        = fd;
    s->events_mask = events_mask;
    s->cb        = cb;
    s->cb_user   = cb_user;
    s->active    = 1;

    int uv_events = 0;
    if (events_mask & YMGUI_FD_READABLE) uv_events |= UV_READABLE;
    if (events_mask & YMGUI_FD_WRITABLE) uv_events |= UV_WRITABLE;
    if (uv_poll_start(&s->poll, uv_events, on_app_fd) != 0) {
        uv_close((uv_handle_t *)&s->poll, on_handle_close);
        s->active = 0;
        return 0;
    }
    return s->id;
}

void ymgui_event_loop_remove_fd(struct ymgui_event_loop *L, int id)
{
    struct loop_fd_slot *s = find_fd_slot(L, id);
    if (!s) return;
    uv_poll_stop(&s->poll);
    uv_close((uv_handle_t *)&s->poll, on_handle_close);
    s->active = 0;
}

/*=============================================================================
 * App extensions: timers
 *===========================================================================*/

static struct loop_timer_slot *find_timer_slot(struct ymgui_event_loop *L, int id)
{
    for (int i = 0; i < YMGUI_LOOP_TIMER_SLOTS; i++)
        if (L->timer_slots[i].active && L->timer_slots[i].id == id)
            return &L->timer_slots[i];
    return NULL;
}

static void on_app_timer(uv_timer_t *handle)
{
    struct loop_timer_slot *s = handle->data;
    if (s->cb) s->cb(s->cb_user);
}

int ymgui_event_loop_add_timer(struct ymgui_event_loop *L,
                               int timeout_ms, int repeat_ms,
                               ymgui_timer_cb cb, void *cb_user)
{
    if (!L || timeout_ms < 0 || !cb) return 0;

    int idx = -1;
    for (int i = 0; i < YMGUI_LOOP_TIMER_SLOTS; i++) {
        if (!L->timer_slots[i].active) { idx = i; break; }
    }
    if (idx < 0) return 0;

    struct loop_timer_slot *s = &L->timer_slots[idx];
    if (uv_timer_init(&L->loop, &s->timer) != 0) return 0;
    s->timer.data = s;
    s->loop       = L;
    s->id         = L->next_timer_id++;
    s->timeout_ms = timeout_ms;
    s->repeat_ms  = repeat_ms;
    s->cb         = cb;
    s->cb_user    = cb_user;
    s->active     = 1;

    if (uv_timer_start(&s->timer, on_app_timer,
                       (uint64_t)timeout_ms,
                       (uint64_t)(repeat_ms > 0 ? repeat_ms : 0)) != 0) {
        uv_close((uv_handle_t *)&s->timer, on_handle_close);
        s->active = 0;
        return 0;
    }
    return s->id;
}

void ymgui_event_loop_remove_timer(struct ymgui_event_loop *L, int id)
{
    struct loop_timer_slot *s = find_timer_slot(L, id);
    if (!s) return;
    uv_timer_stop(&s->timer);
    uv_close((uv_handle_t *)&s->timer, on_handle_close);
    s->active = 0;
}

/*=============================================================================
 * App extensions: posted tasks
 *===========================================================================*/

void ymgui_event_loop_post(struct ymgui_event_loop *L,
                           ymgui_task_cb cb, void *cb_user)
{
    if (!L || !cb) return;
    struct post_node *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->cb      = cb;
    n->cb_user = cb_user;

    uv_mutex_lock(&L->post_mutex);
    if (L->post_tail) L->post_tail->next = n;
    else              L->post_head       = n;
    L->post_tail = n;
    uv_mutex_unlock(&L->post_mutex);

    uv_async_send(&L->post_async);
}
