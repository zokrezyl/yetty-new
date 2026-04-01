// GLFW surface.cpp - WebGPU surface creation from GLFW window
//
// Creates WGPUSurface from GLFWwindow. Uses glfwCreateWindowWGPUSurface which
// handles platform differences (X11, Wayland, Cocoa, Win32) internally.

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

    ydebug("createSurface: Surface created from GLFW window");
    return surface;
}
