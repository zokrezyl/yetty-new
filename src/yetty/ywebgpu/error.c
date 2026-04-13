#include <yetty/webgpu/error.h>
#include <yetty/ytrace.h>

void yetty_webgpu_uncaptured_error_callback(
    WGPUDevice const *device,
    WGPUErrorType type,
    WGPUStringView message,
    void *userdata1,
    void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;

    const char *type_str = "Unknown";
    switch (type) {
        case WGPUErrorType_Validation: type_str = "Validation"; break;
        case WGPUErrorType_OutOfMemory: type_str = "OutOfMemory"; break;
        case WGPUErrorType_Internal: type_str = "Internal"; break;
        case WGPUErrorType_Unknown: type_str = "Unknown"; break;
        default: break;
    }
    yerror("WebGPU %s error: %.*s", type_str, (int)message.length, message.data);
}

WGPUUncapturedErrorCallbackInfo yetty_webgpu_get_error_callback_info(void)
{
    WGPUUncapturedErrorCallbackInfo info = {0};
    info.callback = yetty_webgpu_uncaptured_error_callback;
    return info;
}
