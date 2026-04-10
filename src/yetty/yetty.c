/*
 * yetty.c - Main yetty implementation
 */

#include <yetty/yetty.h>
#include <yetty/term/terminal.h>
#include <yetty/ytrace.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Yetty instance */
struct yetty_yetty {
    struct yetty_context context;
    struct yetty_term_terminal *terminal;

    /* WebGPU state (owned by Yetty) */
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat surface_format;
};

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

    /* Complete context with owned GPU objects */
    yetty->context.gpu_context.app_gpu_context = yetty->context.app_context.app_gpu_context;
    yetty->context.gpu_context.adapter = yetty->adapter;
    yetty->context.gpu_context.device = yetty->device;
    yetty->context.gpu_context.queue = yetty->queue;
    yetty->context.gpu_context.surface_format = yetty->surface_format;

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

    /* Create terminal */
    ydebug("yetty_create: Creating terminal 80x24...");
    struct yetty_term_terminal_result term_res = yetty_term_terminal_create(
        80, 24, &yetty->context);
    if (!YETTY_IS_OK(term_res)) {
        yetty_destroy(yetty);
        return YETTY_ERR(yetty_yetty, "Failed to create terminal");
    }
    yetty->terminal = term_res.value;
    ydebug("yetty_create: Terminal created");

    ydebug("yetty_create: Complete");
    return YETTY_OK(yetty_yetty, yetty);
}

void yetty_destroy(struct yetty_yetty *yetty)
{
    if (!yetty) {
        return;
    }

    if (yetty->terminal) {
        yetty_term_terminal_destroy(yetty->terminal);
    }

    if (yetty->queue) {
        wgpuQueueRelease(yetty->queue);
    }
    if (yetty->device) {
        wgpuDeviceRelease(yetty->device);
    }
    if (yetty->adapter) {
        wgpuAdapterRelease(yetty->adapter);
    }

    free(yetty);
}

struct yetty_core_void_result yetty_run(struct yetty_yetty *yetty)
{
    ydebug("yetty_run: Starting...");

    if (!yetty) {
        ydebug("yetty_run: yetty is null!");
        return YETTY_ERR(yetty_core_void, "yetty is null");
    }

    if (yetty->terminal) {
        ydebug("yetty_run: Calling terminal run...");
        struct yetty_core_void_result res = yetty_term_terminal_run(yetty->terminal);
        ydebug("yetty_run: Terminal run returned, ok=%d", YETTY_IS_OK(res));
        return res;
    }

    ydebug("yetty_run: No terminal, returning OK");
    return YETTY_OK_VOID();
}
