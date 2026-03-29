#include <yetty/yetty.hpp>
#include <yetty/yetty-context.hpp>
#include <yetty/term/terminal.hpp>
#include <yetty/shared-bind-group.hpp>
#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace yetty {

class YettyImpl : public Yetty {
public:
  explicit YettyImpl(const AppContext& appContext)
      : _appContext(appContext) {}

  ~YettyImpl() override {
    delete _terminal;
    delete _sharedBindGroup;
    if (_queue) wgpuQueueRelease(_queue);
    if (_device) wgpuDeviceRelease(_device);
    if (_adapter) wgpuAdapterRelease(_adapter);
    // Note: instance and surface owned by platform, not released here
  }

  const char* typeName() const override { return "Yetty"; }

  Result<void> init() {
    if (auto res = initWebGPU(); !res) {
      return res;
    }

    // Build YettyContext to pass to children
    _yettyContext.appContext = _appContext;
    _yettyContext.gpuContext.appGpuContext = _appContext.gpuContext;
    _yettyContext.gpuContext.adapter = _adapter;
    _yettyContext.gpuContext.device = _device;
    _yettyContext.gpuContext.queue = _queue;
    _yettyContext.gpuContext.surfaceFormat = _surfaceFormat;
    _yettyContext.sharedBindGroup = _sharedBindGroup;

    // Create terminal
    auto termResult = Terminal::create(_yettyContext);
    if (!termResult) {
      return Err<void>("Failed to create Terminal", termResult);
    }
    _terminal = *termResult;
    return Ok();
  }

  Result<void> run() override {
    if (_terminal) {
      return _terminal->run();
    }
    return Ok();
  }

private:
  Result<void> initWebGPU() {
    ydebug("initWebGPU: Starting...");

    // Instance and surface from platform's AppGpuContext
    auto instance = _appContext.gpuContext.instance;
    auto surface = _appContext.gpuContext.surface;

    if (!instance) {
      return Err<void>("No WebGPU instance provided");
    }
    if (!surface) {
      return Err<void>("No WebGPU surface provided");
    }
    ydebug("initWebGPU: Using platform instance and surface");

    // Request adapter
    WGPURequestAdapterOptions adapterOpts = {};
    adapterOpts.compatibleSurface = surface;
    adapterOpts.powerPreference = WGPUPowerPreference_HighPerformance;

    bool adapterReady = false;
    WGPURequestAdapterCallbackInfo adapterCb = {};
    adapterCb.mode = WGPUCallbackMode_AllowSpontaneous;
    adapterCb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                            WGPUStringView, void* userdata1, void* userdata2) {
      if (status == WGPURequestAdapterStatus_Success) {
        *static_cast<WGPUAdapter*>(userdata1) = adapter;
      }
      *static_cast<bool*>(userdata2) = true;
    };
    adapterCb.userdata1 = &_adapter;
    adapterCb.userdata2 = &adapterReady;

    ydebug("initWebGPU: Requesting adapter...");
    wgpuInstanceRequestAdapter(instance, &adapterOpts, adapterCb);

#if defined(__EMSCRIPTEN__)
    while (!adapterReady) {
      emscripten_sleep(10);
    }
#endif

    if (!_adapter) {
      return Err<void>("Failed to get WebGPU adapter");
    }
    ydebug("initWebGPU: Adapter obtained");

    // Log adapter info
    {
      WGPUAdapterInfo info = {};
      if (wgpuAdapterGetInfo(_adapter, &info) == WGPUStatus_Success) {
        auto sv = [](WGPUStringView s) {
          return (s.data && s.length > 0) ? std::string(s.data, s.length) : "(unknown)";
        };
        ydebug("GPU: {} ({}) backend={}", sv(info.device), sv(info.vendor),
               static_cast<int>(info.backendType));
        wgpuAdapterInfoFreeMembers(info);
      }
    }

    // Request device
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(_adapter, &adapterLimits);

    WGPULimits limits = {};
    limits.maxTextureDimension1D = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxTextureDimension2D = std::min(16384u, adapterLimits.maxTextureDimension2D);
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
    limits.maxStorageBufferBindingSize = std::min(
        static_cast<uint64_t>(512 * 1024 * 1024), adapterLimits.maxStorageBufferBindingSize);
    limits.minUniformBufferOffsetAlignment = WGPU_LIMIT_U32_UNDEFINED;
    limits.minStorageBufferOffsetAlignment = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxVertexBuffers = WGPU_LIMIT_U32_UNDEFINED;
    limits.maxBufferSize = std::min(
        static_cast<uint64_t>(1024 * 1024 * 1024), adapterLimits.maxBufferSize);
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

    WGPUDeviceDescriptor deviceDesc = {};
    deviceDesc.label = WGPU_STR("yetty device");
    deviceDesc.requiredLimits = &limits;
    deviceDesc.defaultQueue.label = WGPU_STR("default queue");

    std::string deviceError;
    WGPURequestDeviceCallbackInfo deviceCb = {};
    deviceCb.mode = WGPUCallbackMode_AllowSpontaneous;
    deviceCb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                           WGPUStringView message, void* userdata1, void* userdata2) {
      if (status == WGPURequestDeviceStatus_Success) {
        *static_cast<WGPUDevice*>(userdata1) = device;
      } else {
        auto msg = (message.data && message.length > 0)
            ? std::string(message.data, message.length) : "unknown error";
        *static_cast<std::string*>(userdata2) = msg;
      }
    };
    deviceCb.userdata1 = &_device;
    deviceCb.userdata2 = &deviceError;

    ydebug("initWebGPU: Requesting device...");
    wgpuAdapterRequestDevice(_adapter, &deviceDesc, deviceCb);

#if defined(__EMSCRIPTEN__)
    while (!_device && deviceError.empty()) {
      emscripten_sleep(10);
    }
#endif

    if (!_device) {
      return Err<void>("Failed to get WebGPU device: " + deviceError);
    }
    ydebug("initWebGPU: Device obtained");

    _queue = wgpuDeviceGetQueue(_device);
    ydebug("initWebGPU: Queue obtained");

    // Determine surface format
    if (surface) {
      WGPUSurfaceCapabilities caps = {};
      wgpuSurfaceGetCapabilities(surface, _adapter, &caps);
      if (caps.formatCount > 0) {
        _surfaceFormat = caps.formats[0];
      }
      wgpuSurfaceCapabilitiesFreeMembers(caps);
    } else {
      _surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
    }
    ydebug("initWebGPU: Surface format = {}", static_cast<int>(_surfaceFormat));

    // Create shared bind group (for MSDF font, shared across views)
    auto sharedBgResult = SharedBindGroup::create(_device);
    if (!sharedBgResult) {
      return Err<void>("Failed to create SharedBindGroup");
    }
    _sharedBindGroup = *sharedBgResult;
    ydebug("initWebGPU: SharedBindGroup created");

    ydebug("initWebGPU: Complete");
    return Ok();
  }

  AppContext _appContext;          // COPY of platform's context
  YettyContext _yettyContext;      // Our context to pass to children
  Terminal* _terminal = nullptr;
  SharedBindGroup* _sharedBindGroup = nullptr;

  // WebGPU state (owned by Yetty)
  WGPUAdapter _adapter = nullptr;
  WGPUDevice _device = nullptr;
  WGPUQueue _queue = nullptr;
  WGPUTextureFormat _surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
};

Result<Yetty*> Yetty::createImpl(const AppContext& appContext) {
  auto* yetty = new YettyImpl(appContext);
  if (auto res = yetty->init(); !res) {
    delete yetty;
    return Err<Yetty*>("Yetty init failed", res);
  }
  return Ok(static_cast<Yetty*>(yetty));
}

} // namespace yetty
