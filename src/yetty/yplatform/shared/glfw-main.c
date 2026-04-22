/* glfw-main.c - Application entry point for Linux/macOS/Windows */

#include <GLFW/glfw3.h>
#include <webgpu/webgpu.h>
#include <yetty/yplatform/thread.h>
#include <yetty/yplatform/fs.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <yetty/yetty.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/event.h>
#include <yetty/ycore/event-loop.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/extract-assets.h>
#include <yetty/ytrace.h>

/* Forward declarations - implemented in other platform files */
const char *yetty_platform_get_cache_dir(void);
const char *yetty_platform_get_data_dir(void);
const char *yetty_platform_get_config_dir(void);
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

static int render_thread_func(void *arg)
{
    struct render_thread_args *args = arg;
    struct yetty_core_void_result res = yetty_run(args->yetty);

    args->result = YETTY_IS_OK(res) ? 0 : 1;
    *(args->running) = 0;

    if (args->window)
        glfwPostEmptyEvent();

    return 0;
}

/* Command line options */
static struct option long_options[] = {
    {"config",       required_argument, 0, 'c'},
    {"execute",      required_argument, 0, 'e'},
    {"vnc-server",   no_argument,       0, 's'},
    {"vnc-headless", no_argument,       0, 'H'},
    {"vnc-port",     required_argument, 0, 'p'},
    {"vnc-client",   required_argument, 0, 'C'},
    {"help",         no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c, --config=FILE      Load config from FILE\n");
    fprintf(stderr, "  -e, --execute=CMD      Execute CMD in terminal\n");
    fprintf(stderr, "  -s, --vnc-server       Run VNC server (mirror mode - window + VNC)\n");
    fprintf(stderr, "  -H, --vnc-headless     Run VNC server (headless - no window)\n");
    fprintf(stderr, "  -p, --vnc-port=PORT    VNC server port (default: 5900)\n");
    fprintf(stderr, "  -C, --vnc-client=HOST  Connect as VNC client to HOST[:PORT]\n");
    fprintf(stderr, "  -h, --help             Show this help\n");
}

static void parse_cmdline(int argc, char **argv, struct yetty_config *config)
{
    optind = 1; /* reset getopt */
    int c;
    while ((c = getopt_long(argc, argv, "c:e:sHp:C:h", long_options, NULL)) != -1) {
        switch (c) {
        case 'c':
            /* config file already handled by yetty_config_create */
            break;
        case 'e':
            config->ops->set_string(config, "shell/command", optarg);
            break;
        case 's':
            config->ops->set_string(config, "vnc/server", "true");
            break;
        case 'H':
            config->ops->set_string(config, "vnc/headless", "true");
            break;
        case 'p':
            config->ops->set_string(config, "vnc/port", optarg);
            break;
        case 'C':
            config->ops->set_string(config, "vnc/client", optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    /* Platform paths */
    const char *cache_dir = yetty_platform_get_cache_dir();
    const char *data_dir = yetty_platform_get_data_dir();
    const char *runtime_dir = yetty_platform_get_runtime_dir();

    char shaders_dir[512];
    char fonts_dir[512];
    snprintf(shaders_dir, sizeof(shaders_dir), "%s/shaders", data_dir);
    snprintf(fonts_dir, sizeof(fonts_dir), "%s/fonts", data_dir);

    yplatform_mkdir_p(cache_dir);
    yplatform_mkdir_p(data_dir);
    yplatform_mkdir_p(runtime_dir);
    yplatform_mkdir_p(fonts_dir);

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

    /* Parse command line arguments into config */
    parse_cmdline(argc, argv, config);

    /* Extract assets */
    yetty_platform_extract_assets(config);
    ydebug("main: assets extracted");

    /* Check for headless mode */
    const char *vnc_headless = config->ops->get_string(config, "vnc/headless", NULL);
    int headless = vnc_headless && strcmp(vnc_headless, "true") == 0;

    /* Window dimensions */
    int width = config->ops->get_int(config, "window/width", 1280);
    int height = config->ops->get_int(config, "window/height", 720);

    /* Window (skip for headless mode) */
    GLFWwindow *window = NULL;
    if (!headless) {
        ydebug("main: creating window %dx%d", width, height);
        window = yetty_platform_create_window(width, height, "yetty");
        if (!window) {
            fprintf(stderr, "Failed to create window\n");
            config->ops->destroy(config);
            glfwTerminate();
            return 1;
        }
        ydebug("main: window created");
        yetty_platform_setup_window_callbacks(window);
        ydebug("main: window callbacks set up");
    } else {
        ydebug("main: headless mode, skipping window creation");
    }

    /* Platform input pipe */
    ydebug("main: creating platform input pipe");
    fflush(stderr);
    struct yetty_platform_input_pipe_result pipe_result = yetty_platform_input_pipe_create();
    ydebug("main: platform input pipe created, ok=%d", pipe_result.ok);
    if (!YETTY_IS_OK(pipe_result)) {
        fprintf(stderr, "Failed to create platform input pipe\n");
        if (window)
            yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }
    struct yetty_platform_input_pipe *platform_input_pipe = pipe_result.value;
    if (window)
        glfwSetWindowUserPointer(window, platform_input_pipe);

    /* PTY factory */
    ydebug("main: creating PTY factory");
    struct yetty_platform_pty_factory_result pty_factory_result = yetty_platform_pty_factory_create(config, NULL);
    if (!YETTY_IS_OK(pty_factory_result)) {
        fprintf(stderr, "Failed to create PTY factory\n");
        platform_input_pipe->ops->destroy(platform_input_pipe);
        if (window)
            yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }
    struct yetty_platform_pty_factory *pty_factory = pty_factory_result.value;

    /* WebGPU instance */
    WGPUInstance instance = wgpuCreateInstance(NULL);
    if (!instance) {
        fprintf(stderr, "Failed to create WebGPU instance\n");
        pty_factory->ops->destroy(pty_factory);
        platform_input_pipe->ops->destroy(platform_input_pipe);
        if (window)
            yetty_platform_destroy_window(window);
        config->ops->destroy(config);
        glfwTerminate();
        return 1;
    }

    /* Surface (NULL for headless mode) */
    WGPUSurface surface = NULL;
    if (window) {
        surface = yetty_platform_create_surface(instance, window);
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
    }

    /* App context */
    int fb_width, fb_height;
    if (window) {
        yetty_platform_get_framebuffer_size(window, &fb_width, &fb_height);
    } else {
        /* Headless mode: use configured dimensions */
        fb_width = width;
        fb_height = height;
    }

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
        /* Note: yetty_destroy already released surface if it was configured */
        wgpuInstanceRelease(instance);
        pty_factory->ops->destroy(pty_factory);
        platform_input_pipe->ops->destroy(platform_input_pipe);
        if (window)
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

    ythread_t *render_thread = ythread_create(render_thread_func, &thread_args);

    /* Initial resize event */
    if (window)
        yetty_platform_get_framebuffer_size(window, &fb_width, &fb_height);
    struct yetty_core_event event = {
        .type = YETTY_EVENT_RESIZE,
        .resize = {
            .width = (float)fb_width,
            .height = (float)fb_height
        }
    };
    platform_input_pipe->ops->write(platform_input_pipe, &event, sizeof(event));

    /* OS event loop (headless uses a simple wait loop) */
    if (window) {
        yetty_platform_run_os_event_loop(window, &running);
    } else {
        /* Headless mode: just wait for render thread to finish */
        while (running) {
            usleep(100000); /* 100ms */
        }
    }
    ythread_join(render_thread);

    /* Cleanup - surface is released by yetty_destroy (yetty owns it after configure) */
    ydebug("main: cleanup starting");
    yetty_destroy(yetty);
    ydebug("main: yetty destroyed, releasing instance");
    wgpuInstanceRelease(instance);
    ydebug("main: instance released, destroying pty_factory");
    pty_factory->ops->destroy(pty_factory);
    ydebug("main: pty_factory destroyed");
    if (window)
        glfwSetWindowUserPointer(window, NULL);
    platform_input_pipe->ops->destroy(platform_input_pipe);
    ydebug("main: platform_input_pipe destroyed");
    config->ops->destroy(config);
    ydebug("main: config destroyed");
    if (window) {
        ydebug("main: destroying window");
        yetty_platform_destroy_window(window);
        ydebug("main: window destroyed");
    }
    ydebug("main: calling glfwTerminate");
    glfwTerminate();
    ydebug("main: cleanup complete");

    return thread_args.result;
}
