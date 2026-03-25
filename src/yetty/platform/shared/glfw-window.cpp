// Linux window.cpp - GLFW window creation and management
//
// Creates a GLFW window for Linux/X11/Wayland. The window is created
// on the main thread and used for both rendering (via WebGPU surface)
// and input (via callbacks set up in raw-event-loop.cpp).

#include <yetty/core/core.hpp>
#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>

using namespace yetty;

GLFWwindow* createWindow(Config::Ptr config) {
    // Get window size from config, or use defaults
    int width = config->get<int>("window/width", 1280);
    int height = config->get<int>("window/height", 720);
    std::string title = config->get<std::string>("window/title", "yetty");

    // No OpenGL context - we use WebGPU
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Allow resize
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Create window
    GLFWwindow* window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window) {
        yerror("Window: glfwCreateWindow failed");
        return nullptr;
    }

    ydebug("Window: Created {}x{} \"{}\"", width, height, title);
    return window;
}

void destroyWindow(GLFWwindow* window) {
    if (window) {
        glfwDestroyWindow(window);
        ydebug("Window: Destroyed");
    }
}

void getWindowSize(GLFWwindow* window, int& width, int& height) {
    if (window) {
        glfwGetWindowSize(window, &width, &height);
    } else {
        width = height = 0;
    }
}

void getFramebufferSize(GLFWwindow* window, int& width, int& height) {
    if (window) {
        glfwGetFramebufferSize(window, &width, &height);
    } else {
        width = height = 0;
    }
}

void getContentScale(GLFWwindow* window, float& xscale, float& yscale) {
    if (window) {
        glfwGetWindowContentScale(window, &xscale, &yscale);
    } else {
        xscale = yscale = 1.0f;
    }
}

bool shouldClose(GLFWwindow* window) {
    return window ? glfwWindowShouldClose(window) : true;
}

void setTitle(GLFWwindow* window, const std::string& title) {
    if (window) {
        glfwSetWindowTitle(window, title.c_str());
    }
}
