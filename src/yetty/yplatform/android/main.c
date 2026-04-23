/* Android main - Application entry point using native_app_glue */

#include <android/looper.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <webgpu/webgpu.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <yetty/yetty.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/event.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-factory.h>

#define LOG_TAG "yetty"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Forward declarations */
const char *yetty_yplatform_get_cache_dir(void);
const char *yetty_yplatform_get_runtime_dir(void);
WGPUSurface yetty_yplatform_create_surface_from_window(WGPUInstance instance, ANativeWindow *window);

/* App state */
struct app_state {
    struct android_app *app;
    ANativeWindow *window;
    struct yetty_yetty *yetty;
    struct yetty_yplatform_input_pipe *pipe;
    struct yetty_yconfig *config;
    struct yetty_yplatform_pty_factory *pty_factory;
    WGPUInstance instance;
    WGPUSurface surface;
    pthread_t render_thread;
    int running;
    int initialized;
};

/* Render thread args */
struct render_thread_args {
    struct yetty_yetty *yetty;
    int *running;
};

static void mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void *render_thread_func(void *arg)
{
    struct render_thread_args *args = arg;
    yetty_run(args->yetty);
    *(args->running) = 0;
    free(args);
    return NULL;
}

static void init_yetty(struct app_state *state)
{
    const char *cache_dir;
    const char *runtime_dir;
    struct yetty_yplatform_paths paths;
    struct yetty_yconfig_result config_result;
    struct yetty_yplatform_input_pipe_result pipe_result;
    struct yetty_yplatform_pty_factory_result pty_result;
    struct yetty_app_context ctx;
    struct yetty_yetty_result yetty_result;
    struct render_thread_args *args;
    int32_t width, height;

    if (state->initialized || !state->window)
        return;

    LOGI("Initializing yetty...");

    cache_dir = yetty_yplatform_get_cache_dir();
    runtime_dir = yetty_yplatform_get_runtime_dir();

    mkdir_p(cache_dir);
    mkdir_p(runtime_dir);

    paths.shaders_dir = cache_dir;
    paths.fonts_dir = cache_dir;
    paths.runtime_dir = runtime_dir;
    paths.bin_dir = NULL;

    /* Config */
    config_result = yetty_yconfig_create(0, NULL, &paths);
    if (!YETTY_IS_OK(config_result)) {
        LOGE("Failed to create config");
        return;
    }
    state->config = config_result.value;

    /* Platform input pipe */
    pipe_result = yetty_yplatform_input_pipe_create();
    if (!YETTY_IS_OK(pipe_result)) {
        LOGE("Failed to create input pipe");
        return;
    }
    state->pipe = pipe_result.value;

    /* PTY factory */
    pty_result = yetty_yplatform_pty_factory_create(state->config, NULL);
    if (!YETTY_IS_OK(pty_result)) {
        LOGE("Failed to create PTY factory");
        return;
    }
    state->pty_factory = pty_result.value;

    /* WebGPU instance */
    state->instance = wgpuCreateInstance(NULL);
    if (!state->instance) {
        LOGE("Failed to create WebGPU instance");
        return;
    }

    /* Surface */
    state->surface = yetty_yplatform_create_surface_from_window(state->instance, state->window);
    if (!state->surface) {
        LOGE("Failed to create surface");
        return;
    }

    /* Get window size */
    width = ANativeWindow_getWidth(state->window);
    height = ANativeWindow_getHeight(state->window);

    /* Create Yetty */
    memset(&ctx, 0, sizeof(ctx));
    ctx.app_gpu_context.instance = state->instance;
    ctx.app_gpu_context.surface = state->surface;
    ctx.app_gpu_context.surface_width = (uint32_t)width;
    ctx.app_gpu_context.surface_height = (uint32_t)height;
    ctx.config = state->config;
    ctx.platform_input_pipe = state->pipe;
    ctx.clipboard_manager = NULL;
    ctx.pty_factory = state->pty_factory;

    yetty_result = yetty_create(&ctx);
    if (!YETTY_IS_OK(yetty_result)) {
        LOGE("Failed to create Yetty");
        return;
    }
    state->yetty = yetty_result.value;
    state->initialized = 1;
    state->running = 1;

    /* Start render thread */
    args = malloc(sizeof(struct render_thread_args));
    args->yetty = state->yetty;
    args->running = &state->running;
    pthread_create(&state->render_thread, NULL, render_thread_func, args);

    LOGI("Yetty initialized successfully");
}

static void term_yetty(struct app_state *state)
{
    if (!state->initialized)
        return;

    LOGI("Terminating yetty...");

    state->running = 0;
    if (state->render_thread) {
        pthread_join(state->render_thread, NULL);
        state->render_thread = 0;
    }

    if (state->yetty) {
        yetty_destroy(state->yetty);
        state->yetty = NULL;
    }
    if (state->surface) {
        wgpuSurfaceRelease(state->surface);
        state->surface = NULL;
    }
    if (state->instance) {
        wgpuInstanceRelease(state->instance);
        state->instance = NULL;
    }
    if (state->pty_factory) {
        state->pty_factory->ops->destroy(state->pty_factory);
        state->pty_factory = NULL;
    }
    if (state->pipe) {
        state->pipe->ops->destroy(state->pipe);
        state->pipe = NULL;
    }
    if (state->config) {
        state->config->ops->destroy(state->config);
        state->config = NULL;
    }

    state->initialized = 0;
}

static int32_t handle_input(struct android_app *app, AInputEvent *event)
{
    struct app_state *state = app->userData;
    int32_t type;
    int32_t action;
    float x, y;
    struct yetty_ycore_event ev = {0};

    if (!state->pipe)
        return 0;

    type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        x = AMotionEvent_getX(event, 0);
        y = AMotionEvent_getY(event, 0);

        switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            ev.type = YETTY_EVENT_MOUSE_DOWN;
            ev.mouse.x = x;
            ev.mouse.y = y;
            ev.mouse.button = 0;
            ev.mouse.mods = 0;
            state->pipe->ops->write(state->pipe, &ev, sizeof(ev));
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            ev.type = YETTY_EVENT_MOUSE_MOVE;
            ev.mouse.x = x;
            ev.mouse.y = y;
            state->pipe->ops->write(state->pipe, &ev, sizeof(ev));
            return 1;
        case AMOTION_EVENT_ACTION_UP:
            ev.type = YETTY_EVENT_MOUSE_UP;
            ev.mouse.x = x;
            ev.mouse.y = y;
            ev.mouse.button = 0;
            ev.mouse.mods = 0;
            state->pipe->ops->write(state->pipe, &ev, sizeof(ev));
            return 1;
        }
    }

    return 0;
}

static void handle_cmd(struct android_app *app, int32_t cmd)
{
    struct app_state *state = app->userData;

    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) {
            state->window = app->window;
            init_yetty(state);
        }
        break;

    case APP_CMD_TERM_WINDOW:
        term_yetty(state);
        state->window = NULL;
        break;

    case APP_CMD_WINDOW_RESIZED:
        if (state->pipe && state->window) {
            int32_t w = ANativeWindow_getWidth(state->window);
            int32_t h = ANativeWindow_getHeight(state->window);
            struct yetty_ycore_event ev = {0};
            ev.type = YETTY_EVENT_RESIZE;
            ev.resize.width = (float)w;
            ev.resize.height = (float)h;
            state->pipe->ops->write(state->pipe, &ev, sizeof(ev));
        }
        break;

    case APP_CMD_DESTROY:
        term_yetty(state);
        break;
    }
}

void android_main(struct android_app *app)
{
    struct app_state state = {0};

    state.app = app;
    app->userData = &state;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    LOGI("Android main started");

    while (1) {
        int events;
        struct android_poll_source *source;

        while (ALooper_pollAll(state.running ? 0 : -1, NULL, &events, (void **)&source) >= 0) {
            if (source)
                source->process(app, source);

            if (app->destroyRequested) {
                LOGI("Destroy requested");
                term_yetty(&state);
                return;
            }
        }
    }
}
