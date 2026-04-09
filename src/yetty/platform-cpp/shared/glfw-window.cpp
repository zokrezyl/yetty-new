// Linux window.cpp - GLFW window creation and management
//
// Creates a GLFW window for Linux/X11/Wayland. The window is created
// on the main thread and used for both rendering (via WebGPU surface)
// and input (via callbacks set up in raw-event-loop.cpp).

#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>
#include <string>

GLFWwindow* createWindow(int width, int height, const char* title) {

    // No OpenGL context - we use WebGPU
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Allow resize
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Create window
    GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
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
