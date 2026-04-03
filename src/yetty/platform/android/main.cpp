// Android main.cpp - Application entry point
//
// Threading model:
// - Main thread: Creates everything, runs ALooper event loop
// - Render thread: Calls yetty->run()

#include <yetty/app-context.hpp>
#include <yetty/config.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/core/event.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/yetty.hpp>
#include <ytrace/ytrace.hpp>

#include <android/looper.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <webgpu/webgpu.h>

#include <thread>
#include <atomic>
#include <future>
#include <cmath>

std::string getCacheDir();
std::string getRuntimeDir();
WGPUSurface createSurface(WGPUInstance instance, ANativeWindow* window);

namespace {

struct AppState {
    std::atomic<bool> running{false};
    yetty::core::PlatformInputPipe* pipe = nullptr;
    ANativeWindow* window = nullptr;
    bool pinchActive = false;
    float lastPinchDistance = 0.0f;
    float lastPinchCenterX = 0.0f;
    float lastPinchCenterY = 0.0f;
};

int translateKeyCode(int keyCode) {
    switch (keyCode) {
        case AKEYCODE_ENTER: return 257;
        case AKEYCODE_TAB: return 258;
        case AKEYCODE_DEL: return 259;
        case AKEYCODE_FORWARD_DEL: return 261;
        case AKEYCODE_DPAD_RIGHT: return 262;
        case AKEYCODE_DPAD_LEFT: return 263;
        case AKEYCODE_DPAD_DOWN: return 264;
        case AKEYCODE_DPAD_UP: return 265;
        case AKEYCODE_PAGE_UP: return 266;
        case AKEYCODE_PAGE_DOWN: return 267;
        case AKEYCODE_MOVE_HOME: return 268;
        case AKEYCODE_MOVE_END: return 269;
        case AKEYCODE_ESCAPE: return 256;
        case AKEYCODE_SPACE: return 32;
        default:
            if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z)
                return 'A' + (keyCode - AKEYCODE_A);
            if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9)
                return '0' + (keyCode - AKEYCODE_0);
            return keyCode;
    }
}

void handleMotionEvent(AppState* state, AInputEvent* event) {
    using namespace yetty::core;
    if (!state->pipe) return;

    int32_t action = AMotionEvent_getAction(event);
    int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
    size_t pointerCount = AMotionEvent_getPointerCount(event);

    if (pointerCount == 2) {
        float x0 = AMotionEvent_getX(event, 0), y0 = AMotionEvent_getY(event, 0);
        float x1 = AMotionEvent_getX(event, 1), y1 = AMotionEvent_getY(event, 1);
        float dx = x1 - x0, dy = y1 - y0;
        float distance = std::sqrt(dx * dx + dy * dy);
        float centerX = (x0 + x1) * 0.5f, centerY = (y0 + y1) * 0.5f;

        if (!state->pinchActive) {
            state->pinchActive = true;
            state->lastPinchDistance = distance;
            state->lastPinchCenterX = centerX;
            state->lastPinchCenterY = centerY;
        } else if (actionMasked == AMOTION_EVENT_ACTION_MOVE) {
            float distanceDelta = distance - state->lastPinchDistance;
            if (std::abs(distanceDelta) > 2.0f) {
                Event ev{Event::Type::Scroll};
                ev.scroll = {centerX, centerY, 0.0f, distanceDelta * 0.02f, 2};
                state->pipe->write(&ev, sizeof(ev));
                state->lastPinchDistance = distance;
            }
        }
        return;
    }

    if (state->pinchActive && (actionMasked == AMOTION_EVENT_ACTION_UP ||
                               actionMasked == AMOTION_EVENT_ACTION_POINTER_UP ||
                               actionMasked == AMOTION_EVENT_ACTION_CANCEL)) {
        state->pinchActive = false;
    }

    if (pointerCount == 1 && !state->pinchActive) {
        float x = AMotionEvent_getX(event, 0), y = AMotionEvent_getY(event, 0);
        Event ev;
        switch (actionMasked) {
            case AMOTION_EVENT_ACTION_DOWN:
                ev.type = Event::Type::MouseDown;
                ev.mouse = {x, y, 0, 0};
                state->pipe->write(&ev, sizeof(ev));
                break;
            case AMOTION_EVENT_ACTION_UP:
                ev.type = Event::Type::MouseUp;
                ev.mouse = {x, y, 0, 0};
                state->pipe->write(&ev, sizeof(ev));
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                ev.type = Event::Type::MouseMove;
                ev.mouse = {x, y, 0, 0};
                state->pipe->write(&ev, sizeof(ev));
                break;
        }
    }
}

void handleKeyEvent(AppState* state, AInputEvent* event) {
    using namespace yetty::core;
    if (!state->pipe) return;

    int32_t action = AKeyEvent_getAction(event);
    int32_t keyCode = AKeyEvent_getKeyCode(event);
    int32_t metaState = AKeyEvent_getMetaState(event);

    int mods = 0;
    if (metaState & AMETA_SHIFT_ON) mods |= 1;
    if (metaState & AMETA_CTRL_ON) mods |= 2;
    if (metaState & AMETA_ALT_ON) mods |= 4;

    Event ev;
    ev.type = (action == AKEY_EVENT_ACTION_DOWN) ? Event::Type::KeyDown : Event::Type::KeyUp;
    ev.key = {translateKeyCode(keyCode), mods, 0};
    state->pipe->write(&ev, sizeof(ev));
}

void handleAppCmd(android_app* app, int32_t cmd) {
    auto* state = static_cast<AppState*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            state->window = app->window;
            break;
        case APP_CMD_TERM_WINDOW:
            state->window = nullptr;
            break;
    }
}

int32_t handleInputEvent(android_app* app, AInputEvent* event) {
    auto* state = static_cast<AppState*>(app->userData);
    int32_t eventType = AInputEvent_getType(event);
    if (eventType == AINPUT_EVENT_TYPE_MOTION)
        handleMotionEvent(state, event);
    else if (eventType == AINPUT_EVENT_TYPE_KEY)
        handleKeyEvent(state, event);
    return 1;
}

} // anonymous namespace

void android_main(android_app* app) {
    using namespace yetty;

    AppState state;
    app->userData = &state;
    app->onAppCmd = handleAppCmd;
    app->onInputEvent = handleInputEvent;

    // 1. Config FIRST (before anything else)
    auto cacheDir = getCacheDir();
    auto runtimeDir = getRuntimeDir();
    PlatformPaths paths = {cacheDir.c_str(), cacheDir.c_str(), runtimeDir.c_str(), nullptr};

    auto configResult = Config::create(0, nullptr, &paths);
    if (!configResult) {
        yerror("Failed to create Config: {}", configResult.error().message());
        return;
    }
    auto* config = *configResult;
    ydebug("main: Config created");

    // 2. Wait for window (main thread)
    while (!app->destroyRequested && !state.window) {
        int events;
        android_poll_source* source;
        if (ALooper_pollAll(-1, nullptr, &events, (void**)&source) >= 0)
            if (source) source->process(app, source);
    }
    if (app->destroyRequested) {
        delete config;
        return;
    }
    ydebug("main: Window ready");

    // 3. PlatformInputPipe (main thread)
    auto pipeResult = core::PlatformInputPipe::create();
    if (!pipeResult) {
        yerror("Failed to create PlatformInputPipe: {}", pipeResult.error().message());
        delete config;
        return;
    }
    state.pipe = *pipeResult;
    ydebug("main: PlatformInputPipe created");

    // 4. PtyFactory (main thread)
    auto ptyFactoryResult = PtyFactory::create(config);
    if (!ptyFactoryResult) {
        yerror("Failed to create PtyFactory: {}", ptyFactoryResult.error().message());
        delete state.pipe;
        delete config;
        return;
    }
    auto* ptyFactory = *ptyFactoryResult;
    ydebug("main: PtyFactory created");

    // 5. WebGPU instance + surface (main thread)
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        yerror("Failed to create WebGPU instance");
        delete ptyFactory;
        delete state.pipe;
        delete config;
        return;
    }
    ydebug("main: WebGPU instance created");

    WGPUSurface surface = createSurface(instance, state.window);
    if (!surface) {
        yerror("Failed to create WebGPU surface");
        wgpuInstanceRelease(instance);
        delete ptyFactory;
        delete state.pipe;
        delete config;
        return;
    }
    ydebug("main: WebGPU surface created");

    // 6. Get window dimensions
    int32_t windowWidth = ANativeWindow_getWidth(state.window);
    int32_t windowHeight = ANativeWindow_getHeight(state.window);
    ydebug("main: Window size {}x{}", windowWidth, windowHeight);

    // 7. AppContext + Yetty (main thread)
    AppContext appContext{};
    appContext.config = config;
    appContext.platformInputPipe = state.pipe;
    appContext.ptyFactory = ptyFactory;
    appContext.appGpuContext.instance = instance;
    appContext.appGpuContext.surface = surface;
    appContext.appGpuContext.windowWidth = static_cast<uint32_t>(windowWidth);
    appContext.appGpuContext.windowHeight = static_cast<uint32_t>(windowHeight);

    auto yettyResult = Yetty::create(appContext);
    if (!yettyResult) {
        yerror("Failed to create Yetty: {}", yettyResult.error().message());
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
        delete ptyFactory;
        delete state.pipe;
        delete config;
        return;
    }
    auto* yetty = *yettyResult;
    ydebug("main: Yetty created");

    // 8. Initial resize event
    {
        auto event = core::Event::resizeEvent(
            static_cast<float>(windowWidth),
            static_cast<float>(windowHeight));
        state.pipe->write(&event, sizeof(event));
        ydebug("main: Posted initial resize {}x{}", windowWidth, windowHeight);
    }

    // 9. Render thread - just calls yetty->run()
    std::atomic<bool> running{true};
    std::thread renderThread([yetty, &running]() {
        ydebug("Render thread started");
        auto runResult = yetty->run();
        if (!runResult) {
            yerror("Yetty run failed: {}", runResult.error().message());
        }
        running = false;
        ydebug("Render thread finished");
    });

    // 10. Main thread: ALooper event loop
    state.running = true;
    while (running && !app->destroyRequested) {
        int events;
        android_poll_source* source;
        if (ALooper_pollAll(100, nullptr, &events, (void**)&source) >= 0)
            if (source) source->process(app, source);
    }

    renderThread.join();
    ydebug("main: Render thread joined");

    // Cleanup
    delete yetty;
    wgpuSurfaceRelease(surface);
    wgpuInstanceRelease(instance);
    delete ptyFactory;
    delete state.pipe;
    state.pipe = nullptr;
    delete config;

    ydebug("main: Shutdown complete");
}
