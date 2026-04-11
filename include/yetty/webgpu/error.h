#ifndef YETTY_WEBGPU_ERROR_H
#define YETTY_WEBGPU_ERROR_H

#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uncaptured error callback for WebGPU device - logs errors via ytrace */
void yetty_webgpu_uncaptured_error_callback(
    WGPUDevice const *device,
    WGPUErrorType type,
    WGPUStringView message,
    void *userdata1,
    void *userdata2);

/* Get callback info struct ready to use in device descriptor */
WGPUUncapturedErrorCallbackInfo yetty_webgpu_get_error_callback_info(void);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_WEBGPU_ERROR_H */
