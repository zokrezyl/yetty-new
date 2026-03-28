// GLFW main.cpp - Application entry point for Linux/macOS/Windows
//
// Threading model:
// - Main thread: Creates window; runs OS event loop
// - Render thread: Runs Yetty (runs libuv EventLoop)

#include <GLFW/glfw3.h>
#include <webgpu/webgpu.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>
#include <yetty/app-context.hpp>
#include <yetty/config.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/core/event.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/yetty.hpp>
#include <ytrace/ytrace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

GLFWwindow *createWindow(int width, int height, const char *title);
void destroyWindow(GLFWwindow *window);
WGPUSurface createSurface(WGPUInstance instance, GLFWwindow* window);

void setupWindowCallbacks(GLFWwindow *window);
void runOsEventLoop(GLFWwindow *window, std::atomic<bool> &running);

// Platform-specific path functions - implemented in {linux,macos,windows}/platform-paths.cpp
std::string getCacheDir();
std::string getRuntimeDir();

int main(int argc, char **argv) {
  using namespace yetty;

  // Enable debug logging to stderr
  spdlog::set_default_logger(spdlog::stderr_color_mt("yetty"));
  spdlog::set_level(spdlog::level::debug);

  if (!glfwInit()) {
    yerror("Failed to initialize GLFW");
    return 1;
  }
  struct GlfwGuard {
    ~GlfwGuard() { glfwTerminate(); }
  } glfwGuard;
  ydebug("main: GLFW initialized");

  // Platform paths (using std::filesystem::path for cross-platform separators)
  namespace fs = std::filesystem;
  auto cacheDir = fs::path(getCacheDir());
  auto runtimeDir = fs::path(getRuntimeDir());
  auto shadersDir = (cacheDir / "shaders").string();
  auto fontsDir = (cacheDir / "fonts").string();

  fs::create_directories(cacheDir);
  fs::create_directories(runtimeDir);
  fs::create_directories(fontsDir);

  auto runtimeDirStr = runtimeDir.string();
  PlatformPaths paths = {.shadersDir = shadersDir.c_str(),
                         .fontsDir = fontsDir.c_str(),
                         .runtimeDir = runtimeDirStr.c_str(),
                         .binDir = nullptr};
  ydebug("main: Platform paths created");

  // Config
  auto configResult = Config::create(argc, argv, &paths);
  if (!configResult) {
    yerror("Failed to create Config: {}", configResult.error().message());
    return 1;
  }
  auto *config = *configResult;
  ydebug("main: Config created");

  // Window
  int width = config->get<int>("window/width", 1280);
  int height = config->get<int>("window/height", 720);
  GLFWwindow *window = createWindow(width, height, "yetty");
  if (!window) {
    yerror("Failed to create window");
    delete config;
    return 1;
  }
  ydebug("main: Window created");

  setupWindowCallbacks(window);

  // PlatformInputPipe
  auto pipeResult = core::PlatformInputPipe::create();
  if (!pipeResult) {
    yerror("Failed to create PlatformInputPipe: {}",
           pipeResult.error().message());
    delete config;
    destroyWindow(window);
    return 1;
  }
  auto *platformInputPipe = *pipeResult;
  glfwSetWindowUserPointer(window, platformInputPipe);
  ydebug("main: PlatformInputPipe created");

  // PtyFactory
  auto ptyFactoryResult = PtyFactory::create(config);
  if (!ptyFactoryResult) {
    yerror("Failed to create PtyFactory: {}",
           ptyFactoryResult.error().message());
    delete platformInputPipe;
    delete config;
    destroyWindow(window);
    return 1;
  }
  auto *ptyFactory = *ptyFactoryResult;
  ydebug("main: PtyFactory created");

  // WebGPU instance and surface
  WGPUInstance instance = wgpuCreateInstance(nullptr);
  if (!instance) {
    yerror("Failed to create WebGPU instance");
    delete ptyFactory;
    delete platformInputPipe;
    delete config;
    destroyWindow(window);
    return 1;
  }
  ydebug("main: WebGPU instance created");

  WGPUSurface surface = createSurface(instance, window);
  if (!surface) {
    yerror("Failed to create WebGPU surface");
    wgpuInstanceRelease(instance);
    delete ptyFactory;
    delete platformInputPipe;
    delete config;
    destroyWindow(window);
    return 1;
  }
  ydebug("main: WebGPU surface created");

  // AppContext
  AppContext appContext{};
  appContext.config = config;
  appContext.platformInputPipe = platformInputPipe;
  appContext.ptyFactory = ptyFactory;
  appContext.instance = instance;
  appContext.surface = surface;

  // Yetty
  auto yettyResult = Yetty::create(appContext);
  if (!yettyResult) {
    yerror("Failed to create Yetty: {}", yettyResult.error().message());
    wgpuSurfaceRelease(surface);
    wgpuInstanceRelease(instance);
    delete ptyFactory;
    delete platformInputPipe;
    delete config;
    destroyWindow(window);
    return 1;
  }
  auto *yetty = *yettyResult;
  ydebug("main: Yetty created");

  // Render thread
  std::promise<Result<void>> resultPromise;
  auto resultFuture = resultPromise.get_future();
  std::atomic<bool> running{true};

  std::thread renderThread([yetty, &resultPromise, &running, window]() {
    ydebug("Render thread started");

    auto runResult = yetty->run();
    if (!runResult) {
      yerror("Yetty run failed: {}", runResult.error().message());
      resultPromise.set_value(Err<void>("Yetty run failed", runResult));
    } else {
      resultPromise.set_value(Ok());
    }

    running = false;
    if (window)
      glfwPostEmptyEvent();
    ydebug("Render thread finished");
  });

  // Initial resize event
  {
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    auto event = core::Event::resizeEvent(static_cast<float>(fbWidth),
                                          static_cast<float>(fbHeight));
    platformInputPipe->write(&event, sizeof(event));
    ydebug("main: Posted initial resize {}x{}", fbWidth, fbHeight);
  }

  // OS event loop
  ydebug("main: Starting OS event loop");
  runOsEventLoop(window, running);
  renderThread.join();
  ydebug("main: OS event loop ended");

  // Cleanup
  delete yetty;
  wgpuSurfaceRelease(surface);
  wgpuInstanceRelease(instance);
  delete ptyFactory;
  glfwSetWindowUserPointer(window, nullptr);
  delete platformInputPipe;
  delete config;
  destroyWindow(window);

  ydebug("main: Shutdown complete");
  return resultFuture.get() ? 0 : 1;
}
