/* glfw-main.c - Application entry point for Linux/macOS/Windows */

#include <GLFW/glfw3.h>
#include <webgpu/webgpu.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <yetty/yetty.h>
#include <yetty/config.h>
#include <yetty/core/event.h>
#include <yetty/core/event-loop.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/extract-assets.h>
#include <yetty/ytrace.h>

/* Forward declarations - implemented in other platform files */
const char *yetty_platform_get_cache_dir(void);
const char *yetty_platform_get_runtime_dir(void);

GLFWwindow *yetty_platform_create_window(int width, int height, const char *title);
void yetty_platform_destroy_window(GLFWwindow *window);
void yetty_platform_get_framebuffer_size(GLFWwindow *window, int *width, int *height);
WGPUSurface yetty_platform_create_surface(WGPUInstance instance, GLFWwindow *window);
void yetty_platform_setup_window_callbacks(GLFWwindow *window);
void yetty_platform_run_os_event_loop(GLFWwindow *window, int *running);

/* Render thread args */
struct render_thread_args {
    struct yetty_yetty *yetty;
    int *running;
    GLFWwindow *window;
    int result;
};

static void *render_thread_func(void *arg)
{
    struct render_thread_args *args = arg;
    struct yetty_core_void_result res = yetty_run(args->yetty);

    args->result = YETTY_IS_OK(res) ? 0 : 1;
    *(args->running) = 0;

    if (args->window)
        glfwPostEmptyEvent();

    return NULL;
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

int main(int argc, char **argv)
{
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    /* Platform paths */
    const char *cache_dir = yetty_platform_get_cache_dir();
    const char *runtime_dir = yetty_platform_get_runtime_dir();

    char shaders_dir[512];
    char fonts_dir[512];
    snprintf(shaders_dir, sizeof(shaders_dir), "%s/shaders", cache_dir);
    snprintf(fonts_dir, sizeof(fonts_dir), "%s/fonts", cache_dir);

    mkdir_p(cache_dir);
    mkdir_p(runtime_dir);
    mkdir_p(fonts_dir);

    struct yetty_platform_paths paths = {
        .shaders_dir = shaders_dir,
        .fonts_dir = fonts_dir,
        .runtime_dir = runtime_dir,
        .bin_dir = NULL
    };

    /* Config */
    struct yetty_config_result config_result = yetty_config_create(argc, argv, &paths);
    if (!YETTY_IS_OK(config_result)) {
        fprintf(stderr, "Failed to create config\n");
        glfwTerminate();
        return 1;
    }
    struct yetty_config *config = config_result.value;

    /* Extract assets */
    yetty_platform_extract_assets(config);

    /* Window */
    int width = config->ops->get_int(config, "window/width", 1280);
    int height = config->ops->get_int(config, "window/height", 720);
    GLFWwindow *window = yetty_platform_create_window(width, height, "yetty");
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }

    yetty_platform_setup_window_callbacks(window);

    /* Platform input pipe */
    struct yetty_platform_input_pipe_result pipe_result = yetty_platform_input_pipe_create();
    if (!YETTY_IS_OK(pipe_result)) {
        fprintf(stderr, "Failed to create platform input pipe\n");
        yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }
    struct yetty_platform_input_pipe *platform_input_pipe = pipe_result.value;
    glfwSetWindowUserPointer(window, platform_input_pipe);

    /* PTY factory */
    struct yetty_platform_pty_factory_result pty_factory_result = yetty_platform_pty_factory_create(config, NULL);
    if (!YETTY_IS_OK(pty_factory_result)) {
        fprintf(stderr, "Failed to create PTY factory\n");
        platform_input_pipe->ops->destroy(platform_input_pipe);
        yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }
    struct yetty_platform_pty_factory *pty_factory = pty_factory_result.value;

    /* WebGPU instance and surface */
    WGPUInstance instance = wgpuCreateInstance(NULL);
    if (!instance) {
        fprintf(stderr, "Failed to create WebGPU instance\n");
        pty_factory->ops->destroy(pty_factory);
        platform_input_pipe->ops->destroy(platform_input_pipe);
        yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }

    WGPUSurface surface = yetty_platform_create_surface(instance, window);
    if (!surface) {
        fprintf(stderr, "Failed to create WebGPU surface\n");
        wgpuInstanceRelease(instance);
        pty_factory->ops->destroy(pty_factory);
        platform_input_pipe->ops->destroy(platform_input_pipe);
        yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }

    /* App context */
    int fb_width, fb_height;
    yetty_platform_get_framebuffer_size(window, &fb_width, &fb_height);

    struct yetty_app_context app_context = {
        .app_gpu_context = {
            .instance = instance,
            .surface = surface,
            .surface_width = (uint32_t)fb_width,
            .surface_height = (uint32_t)fb_height
        },
        .config = config,
        .platform_input_pipe = platform_input_pipe,
        .pty_factory = pty_factory
    };

    /* Yetty */
    struct yetty_yetty_result yetty_result = yetty_create(&app_context);
    if (!YETTY_IS_OK(yetty_result)) {
        fprintf(stderr, "Failed to create Yetty\n");
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
        pty_factory->ops->destroy(pty_factory);
        platform_input_pipe->ops->destroy(platform_input_pipe);
        yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }
    struct yetty_yetty *yetty = yetty_result.value;

    /* Render thread */
    int running = 1;
    struct render_thread_args thread_args = {
        .yetty = yetty,
        .running = &running,
        .window = window,
        .result = 0
    };

    pthread_t render_thread;
    pthread_create(&render_thread, NULL, render_thread_func, &thread_args);

    /* Initial resize event */
    yetty_platform_get_framebuffer_size(window, &fb_width, &fb_height);
    struct yetty_core_event event = {
        .type = YETTY_EVENT_RESIZE,
        .resize = {
            .width = (float)fb_width,
            .height = (float)fb_height
        }
    };
    platform_input_pipe->ops->write(platform_input_pipe, &event, sizeof(event));

    /* OS event loop */
    yetty_platform_run_os_event_loop(window, &running);
    pthread_join(render_thread, NULL);

    /* Cleanup - surface is released by yetty_destroy (yetty owns it after configure) */
    ydebug("main: cleanup starting");
    yetty_destroy(yetty);
    ydebug("main: yetty destroyed, releasing instance");
    wgpuInstanceRelease(instance);
    ydebug("main: instance released, destroying pty_factory");
    pty_factory->ops->destroy(pty_factory);
    ydebug("main: pty_factory destroyed");
    glfwSetWindowUserPointer(window, NULL);
    platform_input_pipe->ops->destroy(platform_input_pipe);
    ydebug("main: platform_input_pipe destroyed");
    config->ops->destroy(config);
    ydebug("main: config destroyed, destroying window");
    yetty_platform_destroy_window(window);
    ydebug("main: window destroyed, calling glfwTerminate");
    glfwTerminate();
    ydebug("main: cleanup complete");

    return thread_args.result;
}
