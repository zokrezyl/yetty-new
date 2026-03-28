// GLFW surface.cpp - WebGPU surface creation from GLFW window
//
// Creates WGPUSurface from GLFWwindow. Uses glfwCreateWindowWGPUSurface which
// handles platform differences (X11, Wayland, Cocoa, Win32) internally.
// Instance is created here since glfwCreateWindowWGPUSurface requires one.

#include <webgpu/webgpu.h>
#include <ytrace/ytrace.hpp>
#include <GLFW/glfw3.h>
#include <glfw3webgpu.h>

WGPUSurface createSurface(GLFWwindow* window) {
    if (!window) {
        yerror("createSurface: null window");
        return nullptr;
    }

    // Create instance (required by glfwCreateWindowWGPUSurface)
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        yerror("createSurface: Failed to create WebGPU instance");
        return nullptr;
    }

    WGPUSurface surface = glfwCreateWindowWGPUSurface(instance, window);
    if (!surface) {
        yerror("createSurface: Failed to create surface from GLFW window");
        wgpuInstanceRelease(instance);
        return nullptr;
    }

    // Note: Instance is released here. The surface retains what it needs internally.
    // Yetty will create its own instance for adapter/device creation.
    wgpuInstanceRelease(instance);

    ydebug("createSurface: Surface created from GLFW window");
    return surface;
}
