#ifndef YETTY_PLATFORM_CLIPBOARD_MANAGER_H
#define YETTY_PLATFORM_CLIPBOARD_MANAGER_H

#include <stddef.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_platform_clipboard_manager;

/* Result type */
YETTY_RESULT_DECLARE(yetty_platform_clipboard_manager, struct yetty_platform_clipboard_manager *);

/* Clipboard manager ops */
struct yetty_platform_clipboard_manager_ops {
    void (*destroy)(struct yetty_platform_clipboard_manager *self);
    /* Returns pointer to internal buffer, valid until next get_text or destroy */
    const char *(*get_text)(struct yetty_platform_clipboard_manager *self);
    void (*set_text)(struct yetty_platform_clipboard_manager *self, const char *text, size_t len);
};

/* Clipboard manager base */
struct yetty_platform_clipboard_manager {
    const struct yetty_platform_clipboard_manager_ops *ops;
};

/* Platform-specific create (implemented per platform) */
struct yetty_platform_clipboard_manager_result yetty_platform_clipboard_manager_create(void);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_PLATFORM_CLIPBOARD_MANAGER_H */
