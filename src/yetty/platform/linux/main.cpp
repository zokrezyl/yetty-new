// Linux main.cpp - Application entry point
//
// Threading model:
// - Main thread: Creates window, instance, surface; runs OS event loop
// - Render thread: Runs Yetty (creates adapter/device/queue, runs libuv
// EventLoop)

#include <GLFW/glfw3.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>
#include <yetty/core/event-queue.hpp>
#include <yetty/core/event.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <ytrace/ytrace.hpp>

// Forward declarations from window.cpp
GLFWwindow *createWindow(yetty::Config::Ptr config);
void destroyWindow(GLFWwindow *window);

// Forward declarations from surface.cpp
struct WebGPUContext;
WebGPUContext *createWebGPUContext(GLFWwindow *window);
void destroyWebGPUContext(WebGPUContext *ctx);
WGPUInstance getInstance(WebGPUContext *ctx);
WGPUSurface getSurface(WebGPUContext *ctx);

// Forward declarations from raw-event-loop.cpp
void setupWindowCallbacks(GLFWwindow *window);
void runOsEventLoop(GLFWwindow *window, std::atomic<bool> &running);

// Forward declaration from pty-io.cpp
namespace yetty {
PtyFactory::Ptr createPtyFactory();
}

namespace {

// Get XDG cache directory or fallback
std::string getCacheDir() {
  if (const char *xdg = std::getenv("XDG_CACHE_HOME")) {
    return std::string(xdg) + "/yetty";
  }
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.cache/yetty";
  }
  return "/tmp/yetty";
}

// Get XDG runtime directory or fallback
std::string getRuntimeDir() {
  if (const char *xdg = std::getenv("XDG_RUNTIME_DIR")) {
    return std::string(xdg) + "/yetty";
  }
  return "/tmp/yetty-" + std::to_string(getuid());
}

// Get executable directory
std::filesystem::path getExeDir() {
  char buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return std::filesystem::path(buf).parent_path();
  }
  return ".";
}

} // anonymous namespace

int main(int argc, char **argv) {
  using namespace yetty;

  // 1. Initialize GLFW
  if (!glfwInit()) {
    yerror("Failed to initialize GLFW");
    return 1;
  }
  struct GlfwGuard {
    ~GlfwGuard() { glfwTerminate(); }
  } glfwGuard;
  ydebug("main: GLFW initialized");

  // 2. Create platform paths
  auto exeDir = getExeDir();
  auto cacheDir = getCacheDir();
  auto runtimeDir = getRuntimeDir();

  auto shadersDir = cacheDir + "/shaders";
  auto fontsDir = cacheDir + "/fonts";
  auto runtimeDirStr = runtimeDir;

  std::filesystem::create_directories(cacheDir);
  std::filesystem::create_directories(runtimeDir);
  std::filesystem::create_directories(fontsDir);

  PlatformPaths paths = {
      .shadersDir = shadersDir.c_str(),
      .fontsDir = fontsDir.c_str(),
      .runtimeDir = runtimeDirStr.c_str(),
      .binDir = nullptr // Not used on Linux
  };
  ydebug("main: Platform paths created");

  // 3. Create Config
  auto configResult = Config::create(argc, argv, &paths);
  if (!configResult) {
    yerror("Failed to create Config: {}", configResult.error().message());
    return 1;
  }
  auto config = *configResult;
  ydebug("main: Config created");

  // Check for headless mode
  bool headless = config->get<bool>("vnc/headless", false);
  GLFWwindow *window = nullptr;
  WebGPUContext *gpuCtx = nullptr;
  WGPUInstance instance = nullptr;
  WGPUSurface surface = nullptr;

  if (!headless) {
    // 4. Create window
    window = createWindow(config);
    if (!window) {
      yerror("Failed to create window");
      return 1;
    }
    ydebug("main: Window created");

    // 5. Create WebGPU instance + surface
    gpuCtx = createWebGPUContext(window);
    if (!gpuCtx) {
      yerror("Failed to create WebGPU context");
      destroyWindow(window);
      return 1;
    }
    instance = getInstance(gpuCtx);
    surface = getSurface(gpuCtx);
    ydebug("main: WebGPU instance + surface created");

    // Set up input callbacks
    setupWindowCallbacks(window);
  } else {
    ydebug("main: Headless mode");
  }

  // Initialize EventQueue
  auto queueResult = core::EventQueue::instance();
  if (!queueResult) {
    yerror("Failed to create EventQueue: {}", queueResult.error().message());
    if (gpuCtx)
      destroyWebGPUContext(gpuCtx);
    if (window)
      destroyWindow(window);
    return 1;
  }
  ydebug("main: EventQueue initialized");

  // 6. Create PtyFactory
  auto ptyFactory = createPtyFactory();
  ydebug("main: PtyFactory created");

  // 7. Create Yetty
  auto yettyResult = Yetty::create(config, instance, surface, ptyFactory);
  if (!yettyResult) {
    yerror("Failed to create Yetty: {}", yettyResult.error().message());
    if (gpuCtx)
      destroyWebGPUContext(gpuCtx);
    if (window)
      destroyWindow(window);
    return 1;
  }
  auto yetty = *yettyResult;
  ydebug("main: Yetty created");

  // Result handling
  std::promise<Result<void>> resultPromise;
  auto resultFuture = resultPromise.get_future();
  std::atomic<bool> running{true};

  // 8. Spawn render thread
  std::thread renderThread([&yetty, &resultPromise, &running, window]() {
    ydebug("Render thread started");

    // Get initial surface dimensions
    float width = 0, height = 0;
    if (window) {
      int fbWidth, fbHeight;
      glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
      width = static_cast<float>(fbWidth);
      height = static_cast<float>(fbHeight);
    }

    auto runResult = yetty->run(width, height);
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

  // 9. Post initial resize event with framebuffer size
  if (window) {
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    (*queueResult)->push(core::Event::resizeEvent(
        static_cast<float>(fbWidth), static_cast<float>(fbHeight)));
    ydebug("main: Posted initial resize {}x{}", fbWidth, fbHeight);
  }

  // 10. Run OS event loop
  ydebug("main: Starting OS event loop");
  if (headless) {
    renderThread.join();
  } else {
    runOsEventLoop(window, running);
    renderThread.join();
  }
  ydebug("main: OS event loop ended");

  // Cleanup (reverse order)
  yetty.reset();
  if (gpuCtx)
    destroyWebGPUContext(gpuCtx);
  if (window)
    destroyWindow(window);

  ydebug("main: Shutdown complete");
  return resultFuture.get() ? 0 : 1;
}
