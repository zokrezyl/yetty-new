#include <yetty/yetty.hpp>
#include <yetty/yetty-context.hpp>
#include <yetty/term/terminal.hpp>
#include <yetty/wgpu-compat.hpp>
#include <ytrace/ytrace.hpp>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace yetty {

class YettyImpl : public Yetty {
public:
  explicit YettyImpl(const AppContext &appCtx) : _appCtx(appCtx) {
    _yettyCtx.appCtx = &_appCtx;
  }

  ~YettyImpl() override {
    delete _terminal;
    if (_queue) wgpuQueueRelease(_queue);
    if (_device) wgpuDeviceRelease(_device);
    if (_adapter) wgpuAdapterRelease(_adapter);
    // Note: instance and surface owned by platform, not released here
  }

  const char *typeName() const override { return "Yetty"; }

  Result<void> init() {
    // Initialize WebGPU
    if (auto res = initWebGPU(); !res) {
      return res;
    }

    // Create terminal
    auto termResult = Terminal::create(_yettyCtx);
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

    // Instance and surface provided by platform
    _instance = _appCtx.instance;
    _surface = _appCtx.surface;

    if (!_instance) {
      return Err<void>("No WebGPU instance provided");
    }
    if (!_surface) {
      return Err<void>("No WebGPU surface provided");
    }
    ydebug("initWebGPU: Using platform instance and surface");

    // Request adapter
    WGPURequestAdapterOptions adapterOpts = {};
    adapterOpts.compatibleSurface = _surface;
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
    wgpuInstanceRequestAdapter(_instance, &adapterOpts, adapterCb);

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
    limits.maxTextureDimension2D = std::min(16384u, adapterLimits.maxTextureDimension2D);
    limits.maxStorageBuffersPerShaderStage = 10;
    limits.maxStorageBufferBindingSize = std::min(
        static_cast<uint64_t>(512 * 1024 * 1024), adapterLimits.maxStorageBufferBindingSize);
    limits.maxBufferSize = std::min(
        static_cast<uint64_t>(1024 * 1024 * 1024), adapterLimits.maxBufferSize);

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
    if (_surface) {
      WGPUSurfaceCapabilities caps = {};
      wgpuSurfaceGetCapabilities(_surface, _adapter, &caps);
      if (caps.formatCount > 0) {
        _surfaceFormat = caps.formats[0];
      }
      wgpuSurfaceCapabilitiesFreeMembers(caps);
    } else {
      _surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
    }
    ydebug("initWebGPU: Surface format = {}", static_cast<int>(_surfaceFormat));

    // Populate GPU context
    _yettyCtx.gpuCtx.device = _device;
    _yettyCtx.gpuCtx.queue = _queue;
    _yettyCtx.gpuCtx.surfaceFormat = _surfaceFormat;

    ydebug("initWebGPU: Complete");
    return Ok();
  }

  AppContext _appCtx;
  YettyContext _yettyCtx;
  Terminal *_terminal = nullptr;

  // WebGPU state
  WGPUInstance _instance = nullptr;
  WGPUSurface _surface = nullptr;
  WGPUAdapter _adapter = nullptr;
  WGPUDevice _device = nullptr;
  WGPUQueue _queue = nullptr;
  WGPUTextureFormat _surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
};

Result<Yetty *> Yetty::createImpl(const AppContext &appCtx) {
  auto *yetty = new YettyImpl(appCtx);
  if (auto res = yetty->init(); !res) {
    delete yetty;
    return Err<Yetty *>("Yetty init failed", res);
  }
  return Ok(static_cast<Yetty *>(yetty));
}

} // namespace yetty
