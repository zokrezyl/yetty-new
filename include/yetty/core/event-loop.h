#ifndef YETTY_CORE_EVENT_LOOP_H
#define YETTY_CORE_EVENT_LOOP_H

#include <yetty/core/event.h>
#include <yetty/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int yetty_core_poll_id;
typedef int yetty_core_timer_id;

/* Result types for this module */
YETTY_RESULT_DECLARE(yetty_core_poll_id, yetty_core_poll_id);
YETTY_RESULT_DECLARE(yetty_core_timer_id, yetty_core_timer_id);

#define YETTY_CORE_POLL_READABLE 1
#define YETTY_CORE_POLL_WRITABLE 2

struct yetty_core_event_loop;
struct yetty_core_event_listener;

/* Event listener callback - returns 1 if handled, 0 if not */
typedef int (*yetty_core_event_handler)(
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

    /* Poll management */
    struct yetty_core_poll_id_result (*create_poll)(
        struct yetty_core_event_loop *self);
    struct yetty_core_void_result (*config_poll)(
        struct yetty_core_event_loop *self,
        yetty_core_poll_id id,
        int fd);
    struct yetty_core_void_result (*start_poll)(
        struct yetty_core_event_loop *self,
        yetty_core_poll_id id,
        int events);
    struct yetty_core_void_result (*stop_poll)(
        struct yetty_core_event_loop *self,
        yetty_core_poll_id id);
    struct yetty_core_void_result (*destroy_poll)(
        struct yetty_core_event_loop *self,
        yetty_core_poll_id id);
    struct yetty_core_void_result (*register_poll_listener)(
        struct yetty_core_event_loop *self,
        yetty_core_poll_id id,
        struct yetty_core_event_listener *listener);

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
