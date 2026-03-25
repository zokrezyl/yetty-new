// Linux raw-event-loop.cpp - OS event loop using GLFW
//
// Threading model:
// - This runs on the MAIN THREAD
// - GLFW callbacks are invoked during glfwWaitEvents() on the main thread
// - Callbacks push events to EventQueue, which wakes the render thread

#include <yetty/core/event-queue.hpp>
#include <yetty/core/event.hpp>
#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>
#include <atomic>

using namespace yetty;

namespace {

// GLFW key callback - executes on MAIN THREAD during glfwWaitEvents()
static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)window;
    auto queueResult = core::EventQueue::instance();
    if (!queueResult) return;
    auto queue = *queueResult;

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) {
            if (key == GLFW_KEY_SPACE) {
                queue->push(core::Event::charInputWithMods(' ', mods));
                return;
            }
            const char* keyName = glfwGetKeyName(key, scancode);
            if (keyName && keyName[0] && !keyName[1]) {
                uint32_t ch = static_cast<uint32_t>(static_cast<uint8_t>(keyName[0]));
                queue->push(core::Event::charInputWithMods(ch, mods));
                return;
            }
        }
        queue->push(core::Event::keyDown(key, mods, scancode));
    } else if (action == GLFW_RELEASE) {
        queue->push(core::Event::keyUp(key, mods, scancode));
    }
}

static void charCallback(GLFWwindow* window, unsigned int codepoint) {
    (void)window;
    auto queueResult = core::EventQueue::instance();
    if (!queueResult) return;
    (*queueResult)->push(core::Event::charInput(codepoint));
}

static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    auto queueResult = core::EventQueue::instance();
    if (!queueResult) return;

    double x, y;
    glfwGetCursorPos(window, &x, &y);

    if (action == GLFW_PRESS) {
        (*queueResult)->push(core::Event::mouseDown(
            static_cast<float>(x), static_cast<float>(y), button, mods));
    } else {
        (*queueResult)->push(core::Event::mouseUp(
            static_cast<float>(x), static_cast<float>(y), button, mods));
    }
}

static void cursorPosCallback(GLFWwindow* window, double x, double y) {
    (void)window;
    auto queueResult = core::EventQueue::instance();
    if (!queueResult) return;
    (*queueResult)->push(core::Event::mouseMove(
        static_cast<float>(x), static_cast<float>(y)));
}

static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    auto queueResult = core::EventQueue::instance();
    if (!queueResult) return;

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

    (*queueResult)->push(core::Event::scrollEvent(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(xoffset), static_cast<float>(yoffset), mods));
}

static void framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    (void)window;
    auto queueResult = core::EventQueue::instance();
    if (!queueResult) return;
    (*queueResult)->push(core::Event::resizeEvent(
        static_cast<float>(width), static_cast<float>(height)));
}

static void windowCloseCallback(GLFWwindow* window) {
    (void)window;
    ydebug("Window close requested");
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
