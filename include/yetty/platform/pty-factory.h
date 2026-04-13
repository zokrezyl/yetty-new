#ifndef YETTY_PLATFORM_PTY_FACTORY_H
#define YETTY_PLATFORM_PTY_FACTORY_H

#include <yetty/ycore/result.h>
#include <yetty/platform/pty.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_platform_pty_factory;
struct yetty_config;

/* Result types */
YETTY_RESULT_DECLARE(yetty_platform_pty_factory, struct yetty_platform_pty_factory *);
YETTY_RESULT_DECLARE(yetty_platform_pty, struct yetty_platform_pty *);

/* Pty factory ops */
struct yetty_platform_pty_factory_ops {
    void (*destroy)(struct yetty_platform_pty_factory *self);
    struct yetty_platform_pty_result (*create_pty)(struct yetty_platform_pty_factory *self);
};

/* Pty factory base */
struct yetty_platform_pty_factory {
    const struct yetty_platform_pty_factory_ops *ops;
};

/* Platform-specific create functions (implemented per platform) */
struct yetty_platform_pty_factory_result yetty_platform_pty_factory_create(
    struct yetty_config *config,
    void *os_specific);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_PLATFORM_PTY_FACTORY_H */
