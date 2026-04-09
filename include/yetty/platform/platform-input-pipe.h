#ifndef YETTY_PLATFORM_INPUT_PIPE_H
#define YETTY_PLATFORM_INPUT_PIPE_H

#include <stddef.h>
#include <yetty/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_platform_input_pipe;
struct yetty_core_event_loop;

/* Result type */
YETTY_RESULT_DECLARE(yetty_platform_input_pipe, struct yetty_platform_input_pipe *);

/* Platform input pipe ops */
struct yetty_platform_input_pipe_ops {
    void (*destroy)(struct yetty_platform_input_pipe *self);
    void (*write)(struct yetty_platform_input_pipe *self, const void *data, size_t size);
    size_t (*read)(struct yetty_platform_input_pipe *self, void *data, size_t max_size);
    int (*read_fd)(const struct yetty_platform_input_pipe *self);
    void (*set_event_loop)(struct yetty_platform_input_pipe *self,
                           struct yetty_core_event_loop *loop);
};

/* Platform input pipe base */
struct yetty_platform_input_pipe {
    const struct yetty_platform_input_pipe_ops *ops;
};

/* Platform-specific create */
struct yetty_platform_input_pipe_result yetty_platform_input_pipe_create(void);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_PLATFORM_INPUT_PIPE_H */
