/*
 * yetty.c - Main yetty implementation
 */

#include <yetty/yetty.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/event.h>
#include <yetty/yrender/gpu-allocator.h>
#include <yetty/yrender/render-target.h>
#include <yetty/yterm/terminal.h>
#include <yetty/webgpu/error.h>
#include <yetty/ytrace.h>
#include <yetty/yui/workspace.h>
#include <yetty/yui/tile.h>
#include <yetty/yui/view.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Yetty instance */
struct yetty_yetty {
    struct yetty_context context;
    struct yetty_yui_workspace *workspace;
    struct yetty_core_event_loop *event_loop;
    struct yetty_core_event_listener listener;

    /* WebGPU state (owned by Yetty) */
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat surface_format;

    /* Big render target - window-sized texture with surface for presentation */
    struct yetty_render_target *render_target;
};

/*===========================================================================
 * Event handling
 *===========================================================================*/

static struct yetty_core_int_result yetty_event_handler(
    struct yetty_core_event_listener *listener,
    const struct yetty_core_event *event)
{
    struct yetty_yetty *yetty =
        container_of(listener, struct yetty_yetty, listener);

    /* Handle RENDER event directly - yetty owns the render cycle */
    if (event->type == YETTY_EVENT_RENDER) {
        if (!yetty->render_target) {
            yerror("yetty: RENDER but no render_target");
            return YETTY_OK(yetty_core_int, 0);
        }

        ydebug("yetty: RENDER event - calling workspace render");

        /* Render workspace tree - pass render_target down */
        if (yetty->workspace) {
            struct yetty_core_void_result res =
                yetty_yui_workspace_render(yetty->workspace, yetty->render_target);
            if (!YETTY_IS_OK(res)) {
                yerror("yetty: workspace render failed: %s", res.error.msg);
            }
        }

        /* Present the big target to surface */
        struct yetty_core_void_result res =
            yetty->render_target->ops->present(yetty->render_target);
        if (!YETTY_IS_OK(res)) {
            yerror("yetty: present failed: %s", res.error.msg);
        }

        return YETTY_OK(yetty_core_int, 1);
    }

    /* Handle RESIZE event - reconfigure surface and resize render target */
    if (event->type == YETTY_EVENT_RESIZE) {
        uint32_t width = (uint32_t)event->resize.width;
        uint32_t height = (uint32_t)event->resize.height;

        ydebug("yetty: RESIZE %ux%u", width, height);

        if (width == 0 || height == 0)
            return YETTY_OK(yetty_core_int, 1);

        /* Reconfigure surface */
        WGPUSurface surface = yetty->context.app_context.app_gpu_context.surface;
        if (surface && yetty->device) {
            WGPUSurfaceConfiguration config = {0};
            config.device = yetty->device;
            config.format = yetty->surface_format;
            config.usage = WGPUTextureUsage_RenderAttachment;
            config.width = width;
            config.height = height;
            config.presentMode = WGPUPresentMode_Fifo;
            wgpuSurfaceConfigure(surface, &config);

            yetty->context.app_context.app_gpu_context.surface_width = width;
            yetty->context.app_context.app_gpu_context.surface_height = height;
            yetty->context.gpu_context.app_gpu_context.surface_width = width;
            yetty->context.gpu_context.app_gpu_context.surface_height = height;
        }

        /* Resize render target */
        if (yetty->render_target && yetty->render_target->ops->resize) {
            yetty->render_target->ops->resize(yetty->render_target, width, height);
        }

        /* Resize workspace */
        if (yetty->workspace) {
            yetty_yui_workspace_resize(yetty->workspace,
                (float)width, (float)height);
        }

        /* Forward to workspace for tile/view resize handling */
        if (yetty->workspace)
            yetty_yui_workspace_on_event(yetty->workspace, event);

        return YETTY_OK(yetty_core_int, 1);
    }

    /* Forward other events to workspace */
    if (yetty->workspace)
        return yetty_yui_workspace_on_event(yetty->workspace, event);

    return YETTY_OK(yetty_core_int, 0);
}

static struct yetty_core_void_result register_event_listeners(struct yetty_yetty *yetty)
{
    struct yetty_core_event_loop *el = yetty->event_loop;
    struct yetty_core_void_result res;

    yetty->listener.handler = yetty_event_handler;

    /* Keyboard events */
    res = el->ops->register_listener(el, YETTY_EVENT_KEY_DOWN, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_KEY_UP, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_CHAR, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;

    /* Mouse events */
    res = el->ops->register_listener(el, YETTY_EVENT_MOUSE_DOWN, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_MOUSE_UP, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_MOUSE_MOVE, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_MOUSE_DRAG, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_SCROLL, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;

    /* Other events */
    res = el->ops->register_listener(el, YETTY_EVENT_RESIZE, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_RENDER, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;
    res = el->ops->register_listener(el, YETTY_EVENT_SHUTDOWN, &yetty->listener, 0);
    if (!YETTY_IS_OK(res)) return res;

    ydebug("yetty: registered for all events");
    return YETTY_OK_VOID();
}

/*===========================================================================
 * WebGPU initialization
 *===========================================================================*/

/* Adapter callback data */
struct adapter_callback_data {
    WGPUAdapter *adapter_out;
    int *ready;
};

static void adapter_callback(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                             WGPUStringView message, void *userdata1, void *userdata2)
{
    (void)message;
    if (status == WGPURequestAdapterStatus_Success) {
        *((WGPUAdapter *)userdata1) = adapter;
    }
    *((int *)userdata2) = 1;
}

/* Device callback data */
struct device_callback_data {
    WGPUDevice *device_out;
    char *error_msg;
    size_t error_msg_size;
};

static void device_callback(WGPURequestDeviceStatus status, WGPUDevice device,
                            WGPUStringView message, void *userdata1, void *userdata2)
{
    if (status == WGPURequestDeviceStatus_Success) {
        *((WGPUDevice *)userdata1) = device;
    } else {
        char *error_msg = (char *)userdata2;
        if (message.data && message.length > 0 && error_msg) {
            size_t len = message.length < 255 ? message.length : 255;
            memcpy(error_msg, message.data, len);
            error_msg[len] = '\0';
        }
    }
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static struct yetty_core_void_result init_webgpu(struct yetty_yetty *yetty)
{
    ydebug("initWebGPU: Starting...");

    /* Instance and surface from platform's AppGpuContext */
    WGPUInstance instance = yetty->context.app_context.app_gpu_context.instance;
    WGPUSurface surface = yetty->context.app_context.app_gpu_context.surface;

    if (!instance) {
        return YETTY_ERR(yetty_core_void, "No WebGPU instance provided");
    }
    if (!surface) {
        return YETTY_ERR(yetty_core_void, "No WebGPU surface provided");
    }
    ydebug("initWebGPU: Using platform instance and surface");

    /* Request adapter */
    WGPURequestAdapterOptions adapter_opts = {0};
    adapter_opts.compatibleSurface = surface;
    adapter_opts.powerPreference = WGPUPowerPreference_HighPerformance;

    int adapter_ready = 0;
    WGPURequestAdapterCallbackInfo adapter_cb = {0};
    adapter_cb.mode = WGPUCallbackMode_AllowSpontaneous;
    adapter_cb.callback = adapter_callback;
    adapter_cb.userdata1 = &yetty->adapter;
    adapter_cb.userdata2 = &adapter_ready;

    ydebug("initWebGPU: Requesting adapter...");
    wgpuInstanceRequestAdapter(instance, &adapter_opts, adapter_cb);

    if (!yetty->adapter) {
        return YETTY_ERR(yetty_core_void, "Failed to get WebGPU adapter");
    }
    ydebug("initWebGPU: Adapter obtained");

    /* Log adapter info */
    {
        WGPUAdapterInfo info = {0};
        if (wgpuAdapterGetInfo(yetty->adapter, &info) == WGPUStatus_Success) {
            ydebug("GPU: adapter obtained, backend=%d", (int)info.backendType);
            wgpuAdapterInfoFreeMembers(info);
        }
    }

    /* Request device */
    WGPULimits adapter_limits = {0};
    wgpuAdapterGetLimits(yetty->adapter, &adapter_limits);

    WGPULimits limits = {0};
    limits.maxTextureDimension1D = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxTextureDimension2D = min_u32(16384u, adapter_limits.maxTextureDimension2D);
    limits.maxTextureDimension3D = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxTextureArrayLayers = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxBindGroups = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxBindGroupsPlusVertexBuffers = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxBindingsPerBindGroup = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxDynamicUniformBuffersPerPipelineLayout = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxDynamicStorageBuffersPerPipelineLayout = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxSampledTexturesPerShaderStage = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxSamplersPerShaderStage = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxStorageBuffersPerShaderStage = 10;
    limits.maxStorageTexturesPerShaderStage = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxUniformBuffersPerShaderStage = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxUniformBufferBindingSize = WGPU_LIMIT_U64_UNDEFINED;
    limits.maxStorageBufferBindingSize = min_u64(512ULL * 1024 * 1024, adapter_limits.maxStorageBufferBindingSize);
    limits.minUniformBufferOffsetAlignment = WGPU_LIMIT_U32_UNDEFINED;
    limits.minStorageBufferOffsetAlignment = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxVertexBuffers = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxBufferSize = min_u64(1024ULL * 1024 * 1024, adapter_limits.maxBufferSize);
    limits.maxVertexAttributes = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxVertexBufferArrayStride = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxInterStageShaderVariables = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxColorAttachments = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxColorAttachmentBytesPerSample = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxComputeWorkgroupStorageSize = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxComputeInvocationsPerWorkgroup = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxComputeWorkgroupSizeX = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxComputeWorkgroupSizeY = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxComputeWorkgroupSizeZ = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxComputeWorkgroupsPerDimension = WGPU_LIMIT_U32_UNDEFINED;

    WGPUStringView device_label = {.data = "yetty device", .length = 12};
    WGPUStringView queue_label = {.data = "default queue", .length = 13};

    WGPUDeviceDescriptor device_desc = {0};
    device_desc.label = device_label;
    device_desc.requiredLimits = &limits;
    device_desc.defaultQueue.label = queue_label;
    device_desc.uncapturedErrorCallbackInfo = yetty_webgpu_get_error_callback_info();

    char device_error[256] = {0};
    WGPURequestDeviceCallbackInfo device_cb = {0};
    device_cb.mode = WGPUCallbackMode_AllowSpontaneous;
    device_cb.callback = device_callback;
    device_cb.userdata1 = &yetty->device;
    device_cb.userdata2 = device_error;

    ydebug("initWebGPU: Requesting device...");
    wgpuAdapterRequestDevice(yetty->adapter, &device_desc, device_cb);

    if (!yetty->device) {
        return YETTY_ERR(yetty_core_void, "Failed to get WebGPU device");
    }
    ydebug("initWebGPU: Device obtained");

    yetty->queue = wgpuDeviceGetQueue(yetty->device);
    ydebug("initWebGPU: Queue obtained");

    /* Determine surface format */
    if (surface) {
        WGPUSurfaceCapabilities caps = {0};
        wgpuSurfaceGetCapabilities(surface, yetty->adapter, &caps);
        if (caps.formatCount > 0) {
            yetty->surface_format = caps.formats[0];
        }
        wgpuSurfaceCapabilitiesFreeMembers(caps);
    } else {
        yetty->surface_format = WGPUTextureFormat_BGRA8Unorm;
    }
    ydebug("initWebGPU: Surface format = %d", (int)yetty->surface_format);

    /* Configure surface */
    WGPUSurfaceConfiguration surface_config = {0};
    surface_config.device = yetty->device;
    surface_config.format = yetty->surface_format;
    surface_config.usage = WGPUTextureUsage_RenderAttachment;
    surface_config.width = yetty->context.app_context.app_gpu_context.surface_width;
    surface_config.height = yetty->context.app_context.app_gpu_context.surface_height;
    surface_config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &surface_config);
    ydebug("initWebGPU: Surface configured %ux%u", surface_config.width, surface_config.height);

    /* Create GPU allocator */
    struct yetty_render_gpu_allocator_result alloc_res =
        yetty_render_gpu_allocator_create(yetty->device);
    if (!YETTY_IS_OK(alloc_res)) {
        return YETTY_ERR(yetty_core_void, "failed to create GPU allocator");
    }
    ydebug("initWebGPU: GPU allocator created");

    /* Complete context with owned GPU objects */
    yetty->context.gpu_context.app_gpu_context = yetty->context.app_context.app_gpu_context;
    yetty->context.gpu_context.adapter = yetty->adapter;
    yetty->context.gpu_context.device = yetty->device;
    yetty->context.gpu_context.queue = yetty->queue;
    yetty->context.gpu_context.surface_format = yetty->surface_format;
    yetty->context.gpu_context.allocator = alloc_res.value;

    /* Create big render target (window-sized texture with surface for presentation) */
    struct yetty_render_target_ptr_result target_res = yetty_render_target_texture_create(
        yetty->device,
        yetty->queue,
        yetty->surface_format,
        alloc_res.value,
        surface,  /* surface for presentation */
        yetty->context.app_context.app_gpu_context.surface_width,
        yetty->context.app_context.app_gpu_context.surface_height);
    if (!YETTY_IS_OK(target_res)) {
        return YETTY_ERR(yetty_core_void, "failed to create render target");
    }
    yetty->render_target = target_res.value;
    ydebug("initWebGPU: render target created %ux%u",
        yetty->context.app_context.app_gpu_context.surface_width,
        yetty->context.app_context.app_gpu_context.surface_height);

    ydebug("initWebGPU: Complete");
    return YETTY_OK_VOID();
}

/*===========================================================================
 * Public API
 *===========================================================================*/

struct yetty_yetty_result yetty_create(const struct yetty_app_context *app_context)
{
    ydebug("yetty_create: Starting...");

    struct yetty_yetty *yetty = calloc(1, sizeof(struct yetty_yetty));
    if (!yetty) {
        return YETTY_ERR(yetty_yetty, "Failed to allocate yetty");
    }
    ydebug("yetty_create: Allocated yetty struct");

    /* Copy app context */
    yetty->context.app_context = *app_context;
    ydebug("yetty_create: Copied app context");

    /* Initialize WebGPU */
    struct yetty_core_void_result res = init_webgpu(yetty);
    if (!YETTY_IS_OK(res)) {
        free(yetty);
        return YETTY_ERR(yetty_yetty, "WebGPU init failed");
    }
    ydebug("yetty_create: WebGPU initialized");

    /* Create event loop */
    struct yetty_platform_input_pipe *pipe = app_context->platform_input_pipe;
    struct yetty_core_event_loop_result event_loop_res = yetty_core_event_loop_create(pipe);
    if (!YETTY_IS_OK(event_loop_res)) {
        yetty_destroy(yetty);
        return YETTY_ERR(yetty_yetty, "failed to create event loop");
    }
    yetty->event_loop = event_loop_res.value;
    yetty->context.event_loop = yetty->event_loop;
    ydebug("yetty_create: event loop created at %p", (void *)yetty->event_loop);

    /* Register event listeners */
    res = register_event_listeners(yetty);
    if (!YETTY_IS_OK(res)) {
        yetty_destroy(yetty);
        return YETTY_ERR(yetty_yetty, "failed to register event listeners");
    }

    /* Create workspace */
    ydebug("yetty_create: Creating workspace...");
    struct yetty_yui_workspace_ptr_result ws_res = yetty_yui_workspace_create();
    if (!YETTY_IS_OK(ws_res)) {
        yetty_destroy(yetty);
        return YETTY_ERR(yetty_yetty, "Failed to create workspace");
    }
    yetty->workspace = ws_res.value;
    ydebug("yetty_create: Workspace created");

    /* Load layout from config */
    ydebug("yetty_create: Loading layout from config...");
    struct yetty_core_void_result layout_res = yetty_yui_workspace_load_layout(
        yetty->workspace, app_context->config, &yetty->context);
    if (!YETTY_IS_OK(layout_res)) {
        yetty_destroy(yetty);
        return YETTY_ERR(yetty_yetty, layout_res.error.msg);
    }
    ydebug("yetty_create: Layout loaded");

    ydebug("yetty_create: Complete");
    return YETTY_OK(yetty_yetty, yetty);
}

void yetty_destroy(struct yetty_yetty *yetty)
{
    if (!yetty) {
        return;
    }

    ydebug("yetty_destroy: starting");

    /* Destroy workspace (also destroys tiles and views including terminals) */
    if (yetty->workspace) {
        ydebug("yetty_destroy: destroying workspace");
        yetty_yui_workspace_destroy(yetty->workspace);
        yetty->workspace = NULL;
        ydebug("yetty_destroy: workspace destroyed");
    }

    /* Destroy event loop */
    if (yetty->event_loop && yetty->event_loop->ops &&
        yetty->event_loop->ops->destroy) {
        ydebug("yetty_destroy: destroying event_loop");
        yetty->event_loop->ops->destroy(yetty->event_loop);
        yetty->event_loop = NULL;
        yetty->context.event_loop = NULL;
        ydebug("yetty_destroy: event_loop destroyed");
    }

    /* Destroy render target before allocator */
    if (yetty->render_target && yetty->render_target->ops &&
        yetty->render_target->ops->destroy) {
        ydebug("yetty_destroy: destroying render_target");
        yetty->render_target->ops->destroy(yetty->render_target);
        yetty->render_target = NULL;
    }

    /* Destroy GPU allocator before device */
    if (yetty->context.gpu_context.allocator) {
        ydebug("yetty_destroy: destroying GPU allocator");
        yetty->context.gpu_context.allocator->ops->destroy(
            yetty->context.gpu_context.allocator);
        yetty->context.gpu_context.allocator = NULL;
    }

    /* Surface is created by platform (glfw-main.c) but owned by yetty since we
     * configured it with our device. Must release before device. */
    WGPUSurface surface = yetty->context.app_context.app_gpu_context.surface;
    if (surface && yetty->device) {
        ydebug("yetty_destroy: unconfiguring surface");
        wgpuSurfaceUnconfigure(surface);
        wgpuDeviceTick(yetty->device);
        ydebug("yetty_destroy: releasing surface");
        wgpuSurfaceRelease(surface);
        yetty->context.app_context.app_gpu_context.surface = NULL;
    }

    if (yetty->queue) {
        ydebug("yetty_destroy: releasing queue");
        wgpuQueueRelease(yetty->queue);
    }
    if (yetty->device) {
        ydebug("yetty_destroy: releasing device");
        wgpuDeviceRelease(yetty->device);
    }
    if (yetty->adapter) {
        ydebug("yetty_destroy: releasing adapter");
        wgpuAdapterRelease(yetty->adapter);
    }

    ydebug("yetty_destroy: freeing yetty struct");
    free(yetty);
    ydebug("yetty_destroy: done");
}

struct yetty_core_void_result yetty_run(struct yetty_yetty *yetty)
{
    ydebug("yetty_run: Starting...");

    if (!yetty) {
        ydebug("yetty_run: yetty is null!");
        return YETTY_ERR(yetty_core_void, "yetty is null");
    }

    if (!yetty->event_loop) {
        ydebug("yetty_run: no event_loop!");
        return YETTY_ERR(yetty_core_void, "no event_loop");
    }

    if (!yetty->event_loop->ops || !yetty->event_loop->ops->start) {
        ydebug("yetty_run: event_loop has no start op!");
        return YETTY_ERR(yetty_core_void, "event_loop has no start op");
    }

    ydebug("yetty_run: Starting event loop...");
    struct yetty_core_void_result res = yetty->event_loop->ops->start(yetty->event_loop);
    ydebug("yetty_run: event_loop start returned, ok=%d", YETTY_IS_OK(res));

    return res;
}
