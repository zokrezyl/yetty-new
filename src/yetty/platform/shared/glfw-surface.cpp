// Linux surface.cpp - Platform-specific surface creation
//
// Provides createSurface(instance, window) - creates WGPUSurface from GLFW window.
// Yetty creates instance, calls this via SurfaceCreator, then creates adapter/device/queue.

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>
#include <glfw3webgpu.h>

WGPUSurface createSurface(WGPUInstance instance, GLFWwindow* window) {
    if (!instance || !window) {
        yerror("createSurface: null instance or window");
        return nullptr;
    }

    WGPUSurface surface = glfwCreateWindowWGPUSurface(instance, window);
    if (!surface) {
        yerror("createSurface: Failed to create surface from GLFW window");
        return nullptr;
    }
    ydebug("createSurface: Surface created");

    return surface;
}
