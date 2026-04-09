#ifndef YETTY_PLATFORM_PTY_H
#define YETTY_PLATFORM_PTY_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/core/result.h>
#include <yetty/platform/pty-poll-source.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_platform_pty;

/* Pty ops */
struct yetty_platform_pty_ops {
    void (*destroy)(struct yetty_platform_pty *self);
    size_t (*read)(struct yetty_platform_pty *self, char *buf, size_t max_len);
    void (*write)(struct yetty_platform_pty *self, const char *data, size_t len);
    void (*resize)(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows);
    void (*stop)(struct yetty_platform_pty *self);
    struct yetty_platform_pty_poll_source *(*poll_source)(struct yetty_platform_pty *self);
};

/* Pty base */
struct yetty_platform_pty {
    const struct yetty_platform_pty_ops *ops;
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_PLATFORM_PTY_H */
