#ifndef YETTY_CORE_EVENT_LOOP_H
#define YETTY_CORE_EVENT_LOOP_H

#include <yetty/ycore/event.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int yetty_core_pipe_id;
typedef int yetty_core_timer_id;

/* Result types for this module */
YETTY_RESULT_DECLARE(yetty_core_pipe_id, yetty_core_pipe_id);
YETTY_RESULT_DECLARE(yetty_core_timer_id, yetty_core_timer_id);

struct yetty_core_event_loop;
struct yetty_core_event_listener;
struct yetty_platform_pty_pipe_source;

/* Pipe alloc callback — called by event loop to get a buffer for reading */
typedef void (*yetty_pipe_alloc_cb)(void *ctx, size_t suggested_size, char **buf, size_t *buflen);

/* Pipe read callback — called by event loop when data arrives.
 * nread > 0: data available in buf/nread.
 * nread < 0: error or EOF. */
typedef void (*yetty_pipe_read_cb)(void *ctx, const char *buf, long nread);

/* Event listener callback - returns int (1=handled, 0=not) or error */
typedef struct yetty_core_int_result (*yetty_core_event_handler)(
    struct yetty_core_event_listener *listener,
    const struct yetty_core_event *event
);

/* Event listener - embed as first member in your listener struct */
struct yetty_core_event_listener {
    yetty_core_event_handler handler;
};

/* Event loop ops */
struct yetty_core_event_loop_ops {
    void (*destroy)(struct yetty_core_event_loop *self);

    struct yetty_core_void_result (*start)(struct yetty_core_event_loop *self);
    struct yetty_core_void_result (*stop)(struct yetty_core_event_loop *self);

    struct yetty_core_void_result (*register_listener)(
        struct yetty_core_event_loop *self,
        enum yetty_core_event_type type,
        struct yetty_core_event_listener *listener,
        int priority);
    struct yetty_core_void_result (*deregister_listener)(
        struct yetty_core_event_loop *self,
        enum yetty_core_event_type type,
        struct yetty_core_event_listener *listener);

    struct yetty_core_int_result (*dispatch)(
        struct yetty_core_event_loop *self,
        const struct yetty_core_event *event);
    struct yetty_core_void_result (*broadcast)(
        struct yetty_core_event_loop *self,
        const struct yetty_core_event *event);

    /* PTY pipe — uv_pipe_t with uv_read_start, caller provides callbacks */
    struct yetty_core_pipe_id_result (*register_pty_pipe)(
        struct yetty_core_event_loop *self,
        struct yetty_platform_pty_pipe_source *source,
        yetty_pipe_alloc_cb alloc_cb,
        yetty_pipe_read_cb read_cb,
        void *cb_ctx);
    struct yetty_core_void_result (*unregister_pty_pipe)(
        struct yetty_core_event_loop *self,
        yetty_core_pipe_id id);

    /* Timer management */
    struct yetty_core_timer_id_result (*create_timer)(
        struct yetty_core_event_loop *self);
    struct yetty_core_void_result (*config_timer)(
        struct yetty_core_event_loop *self,
        yetty_core_timer_id id,
        int timeout_ms);
    struct yetty_core_void_result (*start_timer)(
        struct yetty_core_event_loop *self,
        yetty_core_timer_id id);
    struct yetty_core_void_result (*stop_timer)(
        struct yetty_core_event_loop *self,
        yetty_core_timer_id id);
    struct yetty_core_void_result (*destroy_timer)(
        struct yetty_core_event_loop *self,
        yetty_core_timer_id id);
    struct yetty_core_void_result (*register_timer_listener)(
        struct yetty_core_event_loop *self,
        yetty_core_timer_id id,
        struct yetty_core_event_listener *listener);

    void (*request_render)(struct yetty_core_event_loop *self);
};

/* Event loop base */
struct yetty_core_event_loop {
    const struct yetty_core_event_loop_ops *ops;
};

/* Event loop creation - platform_input_pipe can be NULL */
struct yetty_platform_input_pipe;
YETTY_RESULT_DECLARE(yetty_core_event_loop, struct yetty_core_event_loop *);
struct yetty_core_event_loop_result yetty_core_event_loop_create(
    struct yetty_platform_input_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_CORE_EVENT_LOOP_H */
