// WebAssembly main.cpp - Application entry point
//
// Single-threaded model: main() sets up everything and starts the event loop.
// HTML5 callbacks write events to PlatformInputPipe which notifies listeners.

#include <yetty/app-context.hpp>
#include <yetty/config.hpp>
#include <yetty/core/event.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/yetty.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstring>

using namespace yetty;
using namespace yetty::core;

namespace {

//=============================================================================
// Key mapping: DOM code -> GLFW key code
//=============================================================================

int domKeyToGlfw(const char* code, const char* key) {
    (void)key;
    // KeyA-KeyZ -> GLFW_KEY_A-GLFW_KEY_Z (65-90)
    if (code[0] == 'K' && code[1] == 'e' && code[2] == 'y') {
        return 65 + (code[3] - 'A');
    }
    // Digit0-Digit9 -> GLFW_KEY_0-GLFW_KEY_9 (48-57)
    if (code[0] == 'D' && code[1] == 'i' && code[2] == 'g') {
        return 48 + (code[5] - '0');
    }
    // Function keys F1-F12
    if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
        int fnum = code[1] - '0';
        if (code[2] >= '0' && code[2] <= '9') {
            fnum = fnum * 10 + (code[2] - '0');
        }
        return 289 + fnum; // GLFW_KEY_F1 = 290
    }

    // Named keys
    if (!strcmp(code, "Enter")) return 257;
    if (!strcmp(code, "NumpadEnter")) return 335;
    if (!strcmp(code, "Escape")) return 256;
    if (!strcmp(code, "Tab")) return 258;
    if (!strcmp(code, "Backspace")) return 259;
    if (!strcmp(code, "Insert")) return 260;
    if (!strcmp(code, "Delete")) return 261;
    if (!strcmp(code, "ArrowRight")) return 262;
    if (!strcmp(code, "ArrowLeft")) return 263;
    if (!strcmp(code, "ArrowDown")) return 264;
    if (!strcmp(code, "ArrowUp")) return 265;
    if (!strcmp(code, "PageUp")) return 266;
    if (!strcmp(code, "PageDown")) return 267;
    if (!strcmp(code, "Home")) return 268;
    if (!strcmp(code, "End")) return 269;
    if (!strcmp(code, "CapsLock")) return 280;
    if (!strcmp(code, "ScrollLock")) return 281;
    if (!strcmp(code, "NumLock")) return 282;
    if (!strcmp(code, "PrintScreen")) return 283;
    if (!strcmp(code, "Pause")) return 284;
    if (!strcmp(code, "Space")) return 32;
    if (!strcmp(code, "Minus")) return 45;
    if (!strcmp(code, "Equal")) return 61;
    if (!strcmp(code, "BracketLeft")) return 91;
    if (!strcmp(code, "BracketRight")) return 93;
    if (!strcmp(code, "Backslash")) return 92;
    if (!strcmp(code, "Semicolon")) return 59;
    if (!strcmp(code, "Quote")) return 39;
    if (!strcmp(code, "Backquote")) return 96;
    if (!strcmp(code, "Comma")) return 44;
    if (!strcmp(code, "Period")) return 46;
    if (!strcmp(code, "Slash")) return 47;
    if (!strcmp(code, "ShiftLeft")) return 340;
    if (!strcmp(code, "ShiftRight")) return 344;
    if (!strcmp(code, "ControlLeft")) return 341;
    if (!strcmp(code, "ControlRight")) return 345;
    if (!strcmp(code, "AltLeft")) return 342;
    if (!strcmp(code, "AltRight")) return 346;
    if (!strcmp(code, "MetaLeft")) return 343;
    if (!strcmp(code, "MetaRight")) return 347;

    return 0;
}

int domModsToGlfw(const EmscriptenKeyboardEvent* e) {
    int mods = 0;
    if (e->shiftKey) mods |= 0x0001;
    if (e->ctrlKey)  mods |= 0x0002;
    if (e->altKey)   mods |= 0x0004;
    if (e->metaKey)  mods |= 0x0008;
    return mods;
}

int mouseModsToGlfw(const EmscriptenMouseEvent* e) {
    int mods = 0;
    if (e->shiftKey) mods |= 0x0001;
    if (e->ctrlKey)  mods |= 0x0002;
    if (e->altKey)   mods |= 0x0004;
    if (e->metaKey)  mods |= 0x0008;
    return mods;
}

int wheelModsToGlfw(const EmscriptenWheelEvent* e) {
    int mods = 0;
    if (e->mouse.shiftKey) mods |= 0x0001;
    if (e->mouse.ctrlKey)  mods |= 0x0002;
    if (e->mouse.altKey)   mods |= 0x0004;
    if (e->mouse.metaKey)  mods |= 0x0008;
    return mods;
}

//=============================================================================
// HTML5 input callbacks
//=============================================================================

EM_BOOL onKeyDown(int, const EmscriptenKeyboardEvent* e, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    int key = domKeyToGlfw(e->code, e->key);
    int mods = domModsToGlfw(e);
    ydebug("onKeyDown: code='{}' key='{}' glfwKey={} mods={}", e->code, e->key, key, mods);

    // Printable char with Ctrl/Alt -> charInputWithMods
    if ((mods & (0x0002 | 0x0004)) && e->key[0] && !e->key[1]) {
        uint32_t ch = static_cast<uint32_t>(static_cast<uint8_t>(e->key[0]));
        Event event = Event::charInputWithMods(ch, mods);
        pipe->write(&event, sizeof(event));
        ydebug("onKeyDown: sent CharInputWithMods ch={} mods={}", ch, mods);
        return EM_TRUE;
    }

    Event event = Event::keyDown(key, mods, 0);
    pipe->write(&event, sizeof(event));
    ydebug("onKeyDown: sent KeyDown key={}", key);

    // Printable char without Ctrl/Alt -> also send Char event
    if (!(mods & (0x0002 | 0x0004)) && e->key[0] && !e->key[1]) {
        uint32_t ch = static_cast<uint32_t>(static_cast<uint8_t>(e->key[0]));
        Event charEvent = Event::charInput(ch);
        pipe->write(&charEvent, sizeof(charEvent));
        ydebug("onKeyDown: sent CharInput ch={}", ch);
    }

    return EM_TRUE;
}

EM_BOOL onKeyUp(int, const EmscriptenKeyboardEvent* e, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    int key = domKeyToGlfw(e->code, e->key);
    int mods = domModsToGlfw(e);
    Event event = Event::keyUp(key, mods, 0);
    pipe->write(&event, sizeof(event));

    return EM_TRUE;
}

EM_BOOL onMouseDown(int, const EmscriptenMouseEvent* e, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    Event event = Event::mouseDown(
        static_cast<float>(e->targetX),
        static_cast<float>(e->targetY),
        static_cast<int>(e->button),
        mouseModsToGlfw(e));
    pipe->write(&event, sizeof(event));

    return EM_TRUE;
}

EM_BOOL onMouseUp(int, const EmscriptenMouseEvent* e, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    Event event = Event::mouseUp(
        static_cast<float>(e->targetX),
        static_cast<float>(e->targetY),
        static_cast<int>(e->button),
        mouseModsToGlfw(e));
    pipe->write(&event, sizeof(event));

    return EM_TRUE;
}

EM_BOOL onMouseMove(int, const EmscriptenMouseEvent* e, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    Event event = Event::mouseMove(
        static_cast<float>(e->targetX),
        static_cast<float>(e->targetY),
        mouseModsToGlfw(e));
    pipe->write(&event, sizeof(event));

    return EM_FALSE;
}

EM_BOOL onWheel(int, const EmscriptenWheelEvent* e, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    float dx = static_cast<float>(-e->deltaX / 100.0);
    float dy = static_cast<float>(-e->deltaY / 100.0);

    Event event = Event::scrollEvent(
        static_cast<float>(e->mouse.targetX),
        static_cast<float>(e->mouse.targetY),
        dx, dy, wheelModsToGlfw(e));
    pipe->write(&event, sizeof(event));

    return EM_TRUE;
}

EM_BOOL onResize(int, const EmscriptenUiEvent*, void* userData) {
    auto* pipe = static_cast<PlatformInputPipe*>(userData);
    if (!pipe) return EM_FALSE;

    int width = EM_ASM_INT({
        var c = document.getElementById('canvas');
        return c ? c.width : window.innerWidth;
    });
    int height = EM_ASM_INT({
        var c = document.getElementById('canvas');
        return c ? c.height : window.innerHeight;
    });

    Event event = Event::resizeEvent(
        static_cast<float>(width),
        static_cast<float>(height));
    pipe->write(&event, sizeof(event));

    return EM_FALSE;
}

void setupInputCallbacks(PlatformInputPipe* pipe) {
    const char* target = "#canvas";

    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, pipe, true, onKeyDown);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, pipe, true, onKeyUp);
    emscripten_set_mousedown_callback(target, pipe, true, onMouseDown);
    emscripten_set_mouseup_callback(target, pipe, true, onMouseUp);
    emscripten_set_mousemove_callback(target, pipe, true, onMouseMove);
    emscripten_set_wheel_callback(target, pipe, true, onWheel);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, pipe, true, onResize);

    ydebug("main: Input callbacks registered");
}

} // anonymous namespace

//=============================================================================
// Window/surface functions (defined in window.cpp and surface.cpp)
//=============================================================================

namespace yetty {
namespace platform {
namespace webasm {

bool createWindow(Config* config);
void destroyWindow();
void getFramebufferSize(int& width, int& height);

WGPUSurface createSurface(WGPUInstance instance);

} // namespace webasm
} // namespace platform
} // namespace yetty

//=============================================================================
// main
//=============================================================================

int main(int argc, char** argv) {
    spdlog::set_default_logger(spdlog::stderr_color_mt("yetty"));
    spdlog::set_level(spdlog::level::debug);
    ydebug("main: WebASM starting");

    // Platform paths (virtual filesystem)
    PlatformPaths paths = {
        .shadersDir = "/assets/shaders",
        .fontsDir = "/assets/fonts",
        .runtimeDir = "/tmp",
        .binDir = nullptr
    };

    // Config
    auto configResult = Config::create(argc, argv, &paths);
    if (!configResult) {
        yerror("Failed to create Config: {}", configResult.error().message());
        return 1;
    }
    auto* config = *configResult;
    ydebug("main: Config created");

    // Window (canvas)
    if (!platform::webasm::createWindow(config)) {
        yerror("Failed to create window");
        delete config;
        return 1;
    }
    ydebug("main: Window created");

    // PlatformInputPipe
    auto pipeResult = PlatformInputPipe::create();
    if (!pipeResult) {
        yerror("Failed to create PlatformInputPipe: {}", pipeResult.error().message());
        platform::webasm::destroyWindow();
        delete config;
        return 1;
    }
    auto* pipe = *pipeResult;
    ydebug("main: PlatformInputPipe created");

    // Setup HTML5 input callbacks
    setupInputCallbacks(pipe);

    // PtyFactory
    auto ptyFactoryResult = PtyFactory::create(config);
    if (!ptyFactoryResult) {
        yerror("Failed to create PtyFactory: {}", ptyFactoryResult.error().message());
        delete pipe;
        platform::webasm::destroyWindow();
        delete config;
        return 1;
    }
    auto* ptyFactory = *ptyFactoryResult;
    ydebug("main: PtyFactory created");

    // WebGPU instance + surface
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        yerror("Failed to create WebGPU instance");
        delete ptyFactory;
        delete pipe;
        platform::webasm::destroyWindow();
        delete config;
        return 1;
    }
    ydebug("main: WebGPU instance created");

    WGPUSurface surface = platform::webasm::createSurface(instance);
    if (!surface) {
        yerror("Failed to create WebGPU surface");
        wgpuInstanceRelease(instance);
        delete ptyFactory;
        delete pipe;
        platform::webasm::destroyWindow();
        delete config;
        return 1;
    }
    ydebug("main: WebGPU surface created");

    // Get canvas dimensions
    int canvasWidth = EM_ASM_INT({
        var c = document.getElementById('canvas');
        return c ? c.width : window.innerWidth;
    });
    int canvasHeight = EM_ASM_INT({
        var c = document.getElementById('canvas');
        return c ? c.height : window.innerHeight;
    });

    // AppContext
    AppContext appContext{};
    appContext.config = config;
    appContext.platformInputPipe = pipe;
    appContext.ptyFactory = ptyFactory;
    appContext.appGpuContext.instance = instance;
    appContext.appGpuContext.surface = surface;
    appContext.appGpuContext.surfaceWidth = static_cast<uint32_t>(canvasWidth);
    appContext.appGpuContext.surfaceHeight = static_cast<uint32_t>(canvasHeight);

    // Yetty
    auto yettyResult = Yetty::create(appContext);
    if (!yettyResult) {
        yerror("Failed to create Yetty: {}", yettyResult.error().message());
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
        delete ptyFactory;
        delete pipe;
        platform::webasm::destroyWindow();
        delete config;
        return 1;
    }
    auto* yetty = *yettyResult;
    ydebug("main: Yetty created");

    // Initial resize event
    {
        int fbWidth, fbHeight;
        platform::webasm::getFramebufferSize(fbWidth, fbHeight);
        Event event = Event::resizeEvent(
            static_cast<float>(fbWidth),
            static_cast<float>(fbHeight));
        pipe->write(&event, sizeof(event));
        ydebug("main: Posted initial resize {}x{}", fbWidth, fbHeight);
    }

    // Run (event loop starts via emscripten_set_main_loop)
    ydebug("main: Starting Yetty");
    auto runResult = yetty->run();
    if (!runResult) {
        yerror("Yetty run failed: {}", runResult.error().message());
    }

    // Cleanup (won't reach here on web - infinite loop)
    delete yetty;
    wgpuSurfaceRelease(surface);
    wgpuInstanceRelease(instance);
    delete ptyFactory;
    delete pipe;
    platform::webasm::destroyWindow();
    delete config;

    ydebug("main: Shutdown complete");
    return runResult ? 0 : 1;
}
