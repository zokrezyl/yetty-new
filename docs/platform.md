# Platform Abstraction

## Design Principle: Avoid Code Duplication

Platform-specific code is organized to **maximize code reuse**. Instead of duplicating similar logic across platforms, common code lives in `shared/` and each platform directory contains only what's truly unique to that platform.

**Example: GLFW**

GLFW provides window, surface, input, and clipboard handling that works across Linux, macOS, and Windows. Instead of writing separate code for each OS:

```
shared/glfw-window.cpp            ← Window creation (Linux, macOS, Windows)
shared/glfw-surface.cpp           ← WebGPU surface via glfwCreateWindowWGPUSurface()
shared/glfw-event-loop.cpp        ← Event polling
shared/glfw-clipboard-manager.cpp ← Clipboard access
```

**Example: Unix main()**

The main() entry point for Unix-like systems (Linux, macOS) is nearly identical. The only difference is how to determine platform paths (cache, config, runtime directories). So:

```
shared/unix-main.cpp        ← Common main() logic, calls getPlatformPaths()
linux/platform-paths.cpp    ← Implements getPlatformPaths() with XDG
macos/platform-paths.cpp    ← Implements getPlatformPaths() with ~/Library/...
```

The linker picks the correct `getPlatformPaths()` implementation based on the target platform.

**Example: libuv EventLoop**

All native platforms (Linux, macOS, Windows, Android) use libuv for the event loop:

```
shared/libuv-event-loop.cpp ← Used by all native platforms
```

Only WebASM needs a different implementation (emscripten requestAnimationFrame).

## File Structure

```
src/yetty/platform/
├── shared/                      # Reusable code across platforms
│   ├── unix-main.cpp            # main() for Unix (Linux, macOS)
│   ├── unix-pty.cpp             # forkpty() PTY (Linux, macOS)
│   ├── libuv-event-loop.cpp     # Event loop (all native platforms)
│   ├── glfw-window.cpp          # Window creation (Linux, macOS, Windows)
│   ├── glfw-surface.cpp         # WebGPU surface via GLFW (Linux, macOS, Windows)
│   ├── glfw-event-loop.cpp      # GLFW event polling
│   ├── glfw-clipboard-manager.cpp
│   └── fd-pty-poll-source.hpp
│
├── linux/                       # Linux-specific only
│   └── platform-paths.cpp       # XDG directories
│
├── macos/                       # macOS-specific only (planned)
│   └── platform-paths.cpp       # ~/Library/... paths
│
├── windows/                     # Windows-specific only (planned)
│   ├── platform-paths.cpp       # AppData paths
│   └── conpty.cpp               # ConPTY (Windows PTY)
│
├── android/                     # Android-specific only (planned)
│   ├── main.cpp                 # Different entry point (no GLFW)
│   ├── platform-paths.cpp
│   └── surface.cpp              # ANativeWindow surface
│
├── ios/                         # iOS-specific only (planned)
│   ├── main.mm                  # Different entry point (no GLFW)
│   ├── platform-paths.mm
│   └── surface.mm               # CAMetalLayer surface
│
└── webasm/                      # WebASM - mostly unique (no GLFW, no libuv)
    ├── main.cpp                 # Emscripten entry point
    ├── event-loop.cpp           # requestAnimationFrame loop
    ├── surface.cpp              # Canvas-based surface
    ├── window.cpp
    └── pty-io.cpp               # JSLinux PTY via iframe
```

## What Goes Where

| Code | Location | Why |
|------|----------|-----|
| GLFW window/surface/input | `shared/` | Works on Linux, macOS, Windows |
| libuv event loop | `shared/` | Works on all native platforms |
| Unix main() | `shared/` | Same logic for Linux, macOS |
| Unix PTY (forkpty) | `shared/` | Works on Linux, macOS |
| getPlatformPaths() | Each platform dir | Different paths per OS (XDG, ~/Library, AppData) |
| Windows ConPTY | `windows/` | Windows-only PTY API |
| Android/iOS surface | Platform dir | No GLFW, different window system |
| WebASM everything | `webasm/` | No GLFW, no libuv, emscripten-specific |

## Key Abstractions

Platform code implements these interfaces:

| Interface | What it does | Implementations |
|-----------|--------------|-----------------|
| `EventLoop` | Runs main loop | `shared/libuv-event-loop.cpp`, `webasm/event-loop.cpp` |
| `PlatformInputPipe` | Transfers input events | `core/unix-pipe.cpp`, `core/webasm-pipe.cpp` |
| `Pty` + `PtyPollSource` | Shell I/O + polling | `shared/unix-pty.cpp`, `webasm/pty-io.cpp` |
| `getPlatformPaths()` | Cache/config directories | Each platform implements |

### Polling Model: Unix vs WebASM

Unix platforms have file descriptors. libuv polls them. WebASM has no fds - uses in-memory buffers with async callbacks.

**PTY Polling:**

```
include/yetty/platform/pty-poll-source.hpp   ← Base class (opaque handle)
shared/fd-pty-poll-source.hpp                ← Unix: wraps PTY master fd
webasm/pty-io.cpp (WebasmPtyPollSource)      ← WebASM: buffer + JS interop
```

- `Pty::pollSource()` returns a `PtyPollSource*`
- EventLoop static_casts to the concrete type it knows
- Unix: `FdPtyPollSource` wraps fd → libuv polls it
- WebASM: `WebasmPtyPollSource` holds buffer → JS pushes data via postMessage

**PlatformInputPipe Polling:**

```
core/unix-pipe.cpp      ← Unix: real pipe() fd, libuv polls read end
core/webasm-pipe.cpp    ← WebASM: in-memory buffer + emscripten_async_call
```

- Unix: main thread writes to pipe fd, render thread polls read fd via libuv
- WebASM: single thread, write() adds to buffer, schedules async callback to notify listener

## Threading Model

| Platform | Threads | Main Thread | Render Thread |
|----------|---------|-------------|---------------|
| Linux/macOS/Windows | 2 | GLFW event loop | libuv + Yetty |
| Android | 2 | UI events | libuv + Yetty |
| iOS | 2 | UIKit | libuv + Yetty |
| WebASM | 1 | Everything | - |

Desktop threading (2 threads):
```
MAIN THREAD                     RENDER THREAD
───────────────────────────────────────────────
glfwWaitEvents()                libuv event loop
  ↓                               ↓
Input callbacks ──────────────→ PlatformInputPipe
                                  ↓
                                Yetty render
```

## Startup Sequence

1. **getPlatformPaths()** - Platform-specific cache/config/runtime directories
2. **Config::create()** - Parse command line, load config
3. **GLFW init** - Initialize window system (desktop) or canvas (webasm)
4. **Create window** - GLFW window or HTML canvas
5. **Create abstractions** - EventLoop, PlatformInputPipe, PtyFactory
6. **Create WebGPU instance + surface** - Surface needs window handle
7. **Pack into AppContext** - All objects bundled for Yetty
8. **Yetty::create()** - Requests adapter/device, creates Terminal
9. **Run event loop** - Platform-specific main loop

## AppContext

Struct passed from platform main() to Yetty:

```cpp
struct AppContext {
    Config *config;
    EventLoop *eventLoop;
    PlatformInputPipe *platformInputPipe;
    PtyFactory *ptyFactory;
    WGPUInstance instance;
    WGPUSurface surface;
};
```

Platform code creates everything. Yetty just uses it.
