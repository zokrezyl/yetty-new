#ifndef YETTY_YETTY_H
#define YETTY_YETTY_H

#include <yetty/ycore/result.h>
#include <webgpu/webgpu.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct yetty_yetty;
struct yetty_config;
struct yetty_platform_input_pipe;
struct yetty_clipboard_manager;
struct yetty_platform_pty_factory;
struct yetty_core_event_loop;

/* App GPU context - platform-owned GPU objects */
struct yetty_app_gpu_context {
    WGPUInstance instance;
    WGPUSurface surface;
    uint32_t surface_width;
    uint32_t surface_height;
};

/* App context - passed from platform main to yetty_create */
struct yetty_app_context {
    struct yetty_app_gpu_context app_gpu_context;
    struct yetty_config *config;
    struct yetty_platform_input_pipe *platform_input_pipe;
    struct yetty_clipboard_manager *clipboard_manager;
    struct yetty_platform_pty_factory *pty_factory;
};

/* Yetty GPU context - yetty-owned GPU objects */
struct yetty_gpu_context {
    struct yetty_app_gpu_context app_gpu_context;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat surface_format;
};

/* Yetty context - passed down the hierarchy to terminals */
struct yetty_context {
    struct yetty_app_context app_context;
    struct yetty_gpu_context gpu_context;
    struct yetty_core_event_loop *event_loop;
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
