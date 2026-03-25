// Linux main.cpp - Application entry point
//
// Threading model:
// - Main thread: Creates window; runs OS event loop
// - Render thread: Runs Yetty (runs libuv EventLoop)

#include <GLFW/glfw3.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>
#include <yetty/app-context.hpp>
#include <yetty/config.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/core/event.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/yetty.hpp>
#include <ytrace/ytrace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

GLFWwindow *createWindow(int width, int height, const char *title);
void destroyWindow(GLFWwindow *window);

void setupWindowCallbacks(GLFWwindow *window);
void runOsEventLoop(GLFWwindow *window, std::atomic<bool> &running);

namespace {

std::string getCacheDir() {
  if (const char *xdg = std::getenv("XDG_CACHE_HOME")) {
    return std::string(xdg) + "/yetty";
  }
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.cache/yetty";
  }
  return "/tmp/yetty";
}

std::string getRuntimeDir() {
  if (const char *xdg = std::getenv("XDG_RUNTIME_DIR")) {
    return std::string(xdg) + "/yetty";
  }
  return "/tmp/yetty-" + std::to_string(getuid());
}

} // anonymous namespace

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

  // Platform paths
  auto cacheDir = getCacheDir();
  auto runtimeDir = getRuntimeDir();
  auto shadersDir = cacheDir + "/shaders";
  auto fontsDir = cacheDir + "/fonts";

  std::filesystem::create_directories(cacheDir);
  std::filesystem::create_directories(runtimeDir);
  std::filesystem::create_directories(fontsDir);

  PlatformPaths paths = {.shadersDir = shadersDir.c_str(),
                         .fontsDir = fontsDir.c_str(),
                         .runtimeDir = runtimeDir.c_str(),
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

  // EventLoop
  auto eventLoopResult = core::EventLoop::create();
  if (!eventLoopResult) {
    yerror("Failed to create EventLoop: {}",
           eventLoopResult.error().message());
    delete config;
    destroyWindow(window);
    return 1;
  }
  auto *eventLoop = *eventLoopResult;
  ydebug("main: EventLoop created");

  // PlatformInputPipe
  auto pipeResult = core::PlatformInputPipe::create();
  if (!pipeResult) {
    yerror("Failed to create PlatformInputPipe: {}",
           pipeResult.error().message());
    delete eventLoop;
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
    delete eventLoop;
    delete config;
    destroyWindow(window);
    return 1;
  }
  auto *ptyFactory = *ptyFactoryResult;
  ydebug("main: PtyFactory created");

  // AppContext
  AppContext appCtx{};
  appCtx.config = config;
  appCtx.eventLoop = eventLoop;
  appCtx.platformInputPipe = platformInputPipe;
  appCtx.ptyFactory = ptyFactory;

  // Yetty
  auto yettyResult = Yetty::create(appCtx);
  if (!yettyResult) {
    yerror("Failed to create Yetty: {}", yettyResult.error().message());
    delete ptyFactory;
    delete platformInputPipe;
    delete eventLoop;
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
  delete ptyFactory;
  glfwSetWindowUserPointer(window, nullptr);
  delete platformInputPipe;
  delete eventLoop;
  delete config;
  destroyWindow(window);

  ydebug("main: Shutdown complete");
  return resultFuture.get() ? 0 : 1;
}
