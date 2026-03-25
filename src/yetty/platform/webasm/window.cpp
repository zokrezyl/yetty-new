// WebAssembly window.cpp - Canvas element setup and management
//
// On WebAssembly, the "window" is an HTML canvas element. This file handles:
// - Reading the canvas container dimensions
// - Setting the canvas element size
// - Providing dimension/scale queries
//
// Single-threaded: all operations happen on the main (only) thread.

#include <yetty/config.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <string>

namespace yetty {
namespace platform {
namespace webasm {

// Canvas state
static int g_canvasWidth = 0;
static int g_canvasHeight = 0;

bool createWindow(Config::Ptr config) {
    int defaultWidth = config->get<int>("window/width", 1280);
    int defaultHeight = config->get<int>("window/height", 720);
    std::string title = config->get<std::string>("window/title", "yetty");

    // Use actual canvas container size if available, fall back to config defaults
    int containerW = EM_ASM_INT({
        var c = document.getElementById('canvas-container');
        return c ? Math.floor(c.getBoundingClientRect().width) : $0;
    }, defaultWidth);
    int containerH = EM_ASM_INT({
        var c = document.getElementById('canvas-container');
        return c ? Math.floor(c.getBoundingClientRect().height) : $0;
    }, defaultHeight);

    g_canvasWidth = (containerW > 0) ? containerW : defaultWidth;
    g_canvasHeight = (containerH > 0) ? containerH : defaultHeight;

    // Set canvas element size
    emscripten_set_canvas_element_size("#canvas", g_canvasWidth, g_canvasHeight);

    // Set document title
    EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());

    ydebug("Window: Canvas created {}x{} \"{}\"", g_canvasWidth, g_canvasHeight, title);
    return true;
}

void destroyWindow() {
    // Nothing to destroy on web
    ydebug("Window: Destroyed (no-op on web)");
}

void getWindowSize(int& width, int& height) {
    width = g_canvasWidth;
    height = g_canvasHeight;
}

void getFramebufferSize(int& width, int& height) {
    // On web, framebuffer size equals canvas size (device pixel ratio is separate)
    width = g_canvasWidth;
    height = g_canvasHeight;
}

void getContentScale(float& xscale, float& yscale) {
    double ratio = emscripten_get_device_pixel_ratio();
    xscale = yscale = static_cast<float>(ratio);
}

bool shouldClose() {
    return false;  // Web apps don't "close"
}

void setTitle(const std::string& title) {
    EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
}

// Called when canvas container is resized (browser window resize, devtools open/close)
// Updates internal dimensions and canvas element size.
// Returns true if size actually changed.
bool updateCanvasSize() {
    int width = EM_ASM_INT({
        var container = document.getElementById('canvas-container');
        return container ? Math.floor(container.getBoundingClientRect().width) : window.innerWidth;
    });
    int height = EM_ASM_INT({
        var container = document.getElementById('canvas-container');
        return container ? Math.floor(container.getBoundingClientRect().height) : window.innerHeight;
    });

    if (width <= 0 || height <= 0) return false;
    if (width == g_canvasWidth && height == g_canvasHeight) return false;

    g_canvasWidth = width;
    g_canvasHeight = height;
    emscripten_set_canvas_element_size("#canvas", width, height);

    ydebug("Window: Canvas resized to {}x{}", width, height);
    return true;
}

} // namespace webasm
} // namespace platform
} // namespace yetty
