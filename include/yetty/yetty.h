#ifndef YETTY_YETTY_H
#define YETTY_YETTY_H

#include <yetty/core/result.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct yetty_yetty;
struct yetty_config;
struct yetty_platform_input_pipe;
struct yetty_clipboard_manager;
struct yetty_pty_factory;

/* App GPU context - platform-owned GPU objects */
struct yetty_app_gpu_context {
    WGPUInstance instance;
    WGPUSurface surface;
};

/* App context - passed from platform main to yetty_create */
struct yetty_app_context {
    struct yetty_app_gpu_context app_gpu_context;
    struct yetty_config *config;
    struct yetty_platform_input_pipe *platform_input_pipe;
    struct yetty_clipboard_manager *clipboard_manager;
    struct yetty_pty_factory *pty_factory;
};

/* Result type */
YETTY_RESULT_DECLARE(yetty_yetty, struct yetty_yetty *);

/* Create yetty instance */
struct yetty_yetty_result yetty_create(const struct yetty_app_context *app_context);

/* Destroy yetty instance */
void yetty_destroy(struct yetty_yetty *yetty);

/* Run yetty (main loop integration) */
struct yetty_core_void_result yetty_run(struct yetty_yetty *yetty);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YETTY_H */
