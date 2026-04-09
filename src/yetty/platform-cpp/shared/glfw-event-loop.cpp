// glfw-event-loop.cpp - OS event loop using GLFW
//
// Threading model:
// - This runs on the MAIN THREAD
// - GLFW callbacks are invoked during glfwWaitEvents() on the main thread
// - Callbacks write events to PlatformInputPipe, which wakes the render thread
// - PlatformInputPipe pointer is stored via glfwSetWindowUserPointer

#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/core/event.hpp>
#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>
#include <atomic>

using namespace yetty;

namespace {

static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("keyCallback: null platformInputPipe");
        return;
    }

    core::Event event;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) {
            if (key == GLFW_KEY_SPACE) {
                event = core::Event::charInputWithMods(' ', mods);
                pipe->write(&event, sizeof(event));
                return;
            }
            const char* keyName = glfwGetKeyName(key, scancode);
            if (keyName && keyName[0] && !keyName[1]) {
                uint32_t ch = static_cast<uint32_t>(static_cast<uint8_t>(keyName[0]));
                event = core::Event::charInputWithMods(ch, mods);
                pipe->write(&event, sizeof(event));
                return;
            }
        }
        event = core::Event::keyDown(key, mods, scancode);
    } else if (action == GLFW_RELEASE) {
        event = core::Event::keyUp(key, mods, scancode);
    } else {
        return;
    }
    pipe->write(&event, sizeof(event));
}

static void charCallback(GLFWwindow* window, unsigned int codepoint) {
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("charCallback: null platformInputPipe");
        return;
    }
    auto event = core::Event::charInput(codepoint);
    pipe->write(&event, sizeof(event));
}

static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("mouseButtonCallback: null platformInputPipe");
        return;
    }

    double x, y;
    glfwGetCursorPos(window, &x, &y);

    core::Event event;
    if (action == GLFW_PRESS) {
        event = core::Event::mouseDown(
            static_cast<float>(x), static_cast<float>(y), button, mods);
    } else {
        event = core::Event::mouseUp(
            static_cast<float>(x), static_cast<float>(y), button, mods);
    }
    pipe->write(&event, sizeof(event));
}

static void cursorPosCallback(GLFWwindow* window, double x, double y) {
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("cursorPosCallback: null platformInputPipe");
        return;
    }
    auto event = core::Event::mouseMove(static_cast<float>(x), static_cast<float>(y));
    pipe->write(&event, sizeof(event));
}

static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("scrollCallback: null platformInputPipe");
        return;
    }

    double x, y;
    glfwGetCursorPos(window, &x, &y);

    int mods = 0;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
        mods |= GLFW_MOD_SHIFT;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
        mods |= GLFW_MOD_CONTROL;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) {
        mods |= GLFW_MOD_ALT;
    }

    auto event = core::Event::scrollEvent(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(xoffset), static_cast<float>(yoffset), mods);
    pipe->write(&event, sizeof(event));
}

static void framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    ydebug("framebufferSizeCallback: {}x{}", width, height);
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("framebufferSizeCallback: null platformInputPipe");
        return;
    }
    auto event = core::Event::resizeEvent(static_cast<float>(width), static_cast<float>(height));
    pipe->write(&event, sizeof(event));
    ydebug("framebufferSizeCallback: event written");
}

static void windowCloseCallback(GLFWwindow* window) {
    (void)window;
    ydebug("Window close requested");
}

static void windowRefreshCallback(GLFWwindow* window) {
    auto* pipe = static_cast<core::PlatformInputPipe*>(glfwGetWindowUserPointer(window));
    if (!pipe) {
        yerror("windowRefreshCallback: null platformInputPipe");
        return;
    }
    auto event = core::Event::renderEvent();
    pipe->write(&event, sizeof(event));
    ydebug("windowRefreshCallback: render event written");
}

} // anonymous namespace

// Set up all GLFW callbacks for the window
void setupWindowCallbacks(GLFWwindow* window) {
    glfwSetKeyCallback(window, keyCallback);
    glfwSetCharCallback(window, charCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetWindowCloseCallback(window, windowCloseCallback);
    glfwSetWindowRefreshCallback(window, windowRefreshCallback);
    ydebug("OS event loop: Callbacks set up");
}

// Run the OS event loop - blocks until running becomes false
void runOsEventLoop(GLFWwindow* window, std::atomic<bool>& running) {
    ydebug("OS event loop: Starting");

    while (running && !glfwWindowShouldClose(window)) {
        glfwWaitEvents();
    }

    ydebug("OS event loop: Ended");
}
