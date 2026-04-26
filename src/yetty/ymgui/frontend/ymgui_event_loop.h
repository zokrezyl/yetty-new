/*
 * ymgui_event_loop.h — libuv-backed event loop for ymgui frontend apps.
 *
 * Same shape as a GLFW backend, just driven by libuv instead of an OS
 * windowing layer:
 *
 *   - the loop owns stdin/stdout I/O (uv_poll_t on each)
 *   - bytes from yetty are streamed through yface, which fires typed
 *     callbacks for OSC events (mouse / resize / …) and forwards
 *     non-envelope bytes verbatim through on_raw (keyboard / CSI)
 *   - the loop coalesces input arrivals into a single on_frame tick so
 *     the app does NewFrame/Render once per batch
 *   - app code can register its own fds, timers, and posted tasks for
 *     network or whatever else
 *
 * The loop is intentionally scoped to ymgui/frontend for now. If yetty's
 * server-side yetty_ycore_event_loop eventually exposes a client-side
 * shape too, this can fold into it; until then keeping it separate avoids
 * dragging server-only abstractions (PTY pipe sources, render-request,
 * …) into client apps.
 */

#ifndef YETTY_YMGUI_FRONTEND_EVENT_LOOP_H
#define YETTY_YMGUI_FRONTEND_EVENT_LOOP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ymgui_event_loop;

/*=============================================================================
 * Input callbacks (GLFW-style: one setter per event type)
 *
 * Coordinates are pixel-precise relative to the terminal's view origin.
 * Buttons follow ImGui ordering: 0=left, 1=right, 2=middle, 3=x1, 4=x2.
 *===========================================================================*/
typedef void (*ymgui_mouse_pos_cb)   (void *user, double x, double y,
                                      uint32_t buttons_held);
typedef void (*ymgui_mouse_button_cb)(void *user, int button, int pressed,
                                      double x, double y);
typedef void (*ymgui_mouse_wheel_cb) (void *user, double dy,
                                      double x, double y);
typedef void (*ymgui_resize_cb)      (void *user, double width, double height);

/* Raw passthrough — bytes outside any OSC envelope. The app plugs
 * their keyboard / CSI parser here. May be NULL to drop them. */
typedef void (*ymgui_raw_cb)         (void *user, const char *bytes, size_t n);

/* "A batch of input was drained — your ImGui state may have changed,
 * draw a frame." Coalesces multiple input events into one tick. */
typedef void (*ymgui_frame_cb)       (void *user);

/*=============================================================================
 * App extensions (network fds, timers, posted tasks)
 *===========================================================================*/
#define YMGUI_FD_READABLE 0x1
#define YMGUI_FD_WRITABLE 0x2

typedef void (*ymgui_fd_cb)    (void *user, int fd, int events_mask);
typedef void (*ymgui_timer_cb) (void *user);
typedef void (*ymgui_task_cb)  (void *user);

/*=============================================================================
 * Lifecycle
 *===========================================================================*/

struct ymgui_event_loop_config {
    int   in_fd;     /* if <0, defaults to STDIN_FILENO */
    int   out_fd;    /* if <0, defaults to STDOUT_FILENO */
    void *user;      /* opaque, passed to all callbacks */
};

/* Create a loop. Allocates the libuv loop, opens uv_poll on in_fd, and
 * spins up a yface for stream decoding. Does NOT start running yet —
 * call ymgui_event_loop_run / poll. */
struct ymgui_event_loop *ymgui_event_loop_create(
    const struct ymgui_event_loop_config *cfg);
void ymgui_event_loop_destroy(struct ymgui_event_loop *loop);

/*=============================================================================
 * Callback wiring
 *===========================================================================*/
void ymgui_event_loop_set_user           (struct ymgui_event_loop *, void *user);
void ymgui_event_loop_set_mouse_pos_cb   (struct ymgui_event_loop *, ymgui_mouse_pos_cb);
void ymgui_event_loop_set_mouse_button_cb(struct ymgui_event_loop *, ymgui_mouse_button_cb);
void ymgui_event_loop_set_mouse_wheel_cb (struct ymgui_event_loop *, ymgui_mouse_wheel_cb);
void ymgui_event_loop_set_resize_cb      (struct ymgui_event_loop *, ymgui_resize_cb);
void ymgui_event_loop_set_raw_cb         (struct ymgui_event_loop *, ymgui_raw_cb);
void ymgui_event_loop_set_frame_cb       (struct ymgui_event_loop *, ymgui_frame_cb);

/*=============================================================================
 * Run modes
 *===========================================================================*/

/* Block forever, draining I/O and dispatching callbacks, until
 * ymgui_event_loop_stop is called from any thread. Returns 0 on clean
 * exit, non-zero on libuv error. */
int  ymgui_event_loop_run (struct ymgui_event_loop *loop);

/* Pump one iteration. wait != 0 → block until at least one handle
 * fires; wait == 0 → return immediately if nothing ready. Mirrors
 * uv_run UV_RUN_ONCE / UV_RUN_NOWAIT. */
int  ymgui_event_loop_poll(struct ymgui_event_loop *loop, int wait);

/* Wake the loop and ask it to exit. Thread-safe — uses uv_async. */
void ymgui_event_loop_stop(struct ymgui_event_loop *loop);

/* Coalescing wakeup — request that the next iteration call frame_cb
 * even if no input arrives. Thread-safe. Useful when the app's own
 * state changed (e.g., a network reply landed) and the UI needs a
 * redraw. */
void ymgui_event_loop_request_frame(struct ymgui_event_loop *loop);

/*=============================================================================
 * App extensions — return a handle id (>0) or 0 on failure.
 *===========================================================================*/

int  ymgui_event_loop_add_fd   (struct ymgui_event_loop *loop,
                                int fd, int events_mask,
                                ymgui_fd_cb cb, void *cb_user);
void ymgui_event_loop_remove_fd(struct ymgui_event_loop *loop, int id);

int  ymgui_event_loop_add_timer   (struct ymgui_event_loop *loop,
                                   int timeout_ms, int repeat_ms,
                                   ymgui_timer_cb cb, void *cb_user);
void ymgui_event_loop_remove_timer(struct ymgui_event_loop *loop, int id);

/* Schedule fn(arg) to run on the loop thread. Thread-safe. */
void ymgui_event_loop_post(struct ymgui_event_loop *loop,
                           ymgui_task_cb cb, void *cb_user);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMGUI_FRONTEND_EVENT_LOOP_H */
