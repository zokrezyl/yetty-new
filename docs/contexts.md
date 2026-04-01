# Context Pattern

## Purpose

Contexts avoid passing many arguments to factory methods. Instead of:
```cpp
Thing::create(device, queue, allocator, config, font, ...)  // NO
```

We use:
```cpp
Thing::create(const ParentContext& ctx)  // YES
```

## Rules

### 1. Contexts are structs, stored by value

Contexts are plain structs with POD-like semantics. They are cheap to copy.

### 2. Passed by const reference, stored by value

Factory methods receive parent's context by const reference.
The object stores a COPY (by value).

```cpp
class TerminalImpl {
    YettyContext _yettyContext;  // stored by VALUE (copy)

    explicit TerminalImpl(const YettyContext& ctx)
        : _yettyContext(ctx) {}  // copy
};
```

### 3. Parent passes ITS context to child

The child receives the PARENT's context type, not its own.
The child builds its own internal state from the parent context.

```cpp
// Platform creates AppContext, passes to Yetty
Yetty::create(const AppContext& appContext);

// Yetty creates YettyContext, passes to Terminal
Terminal::create(const YettyContext& yettyContext);
```

### 4. Context hierarchy

Each level's context contains the parent's context (by value):

```
AppContext
    ├── AppGpuContext { instance, surface }
    ├── Config*
    ├── paths...
    │
    ▼
YettyContext {
    AppContext appContext;           // copy of parent
    YettyGpuContext gpuContext;      // Yetty-level GPU state
    SharedBindGroup* sharedBindGroup;
}
    │
    ▼
TerminalScreenContext {
    YettyContext yettyContext;              // copy of parent
    TerminalScreenGpuContext gpuContext;    // TerminalScreen-level GPU state
    Pty* pty;
}
```

## GPU Context Hierarchy

Each level has its own GPU context type, containing parent's GPU context plus its additions:

### AppGpuContext (Platform level)
```cpp
struct AppGpuContext {
    WGPUInstance instance;    // created by platform
    WGPUSurface surface;      // created by platform
};
```

### YettyGpuContext (Yetty level)
```cpp
struct YettyGpuContext {
    AppGpuContext appGpuContext;     // copy of parent's GPU context
    WGPUAdapter adapter;              // created by Yetty
    WGPUDevice device;                // created by Yetty
    WGPUQueue queue;                  // created by Yetty
    WGPUTextureFormat surfaceFormat;  // determined by Yetty
};
```

### TerminalScreenGpuContext (TerminalScreen level)
```cpp
struct TerminalScreenGpuContext {
    YettyGpuContext yettyGpuContext;  // copy of parent's GPU context
    GpuAllocator* allocator;           // created by TerminalScreen
    ShaderManager* shaderManager;      // created by TerminalScreen
    // ... other per-view GPU resources
};
```

## Accessing GPU state

```cpp
void TerminalScreen::doSomething() {
    // Access device (from Yetty level)
    auto device = _gpuContext.yettyGpuContext.device;

    // Access surface (from App level)
    auto surface = _gpuContext.yettyGpuContext.appGpuContext.surface;

    // Access own allocator
    auto* allocator = _gpuContext.allocator;
}
```

## Full Context Hierarchy

```
AppContext {
    AppGpuContext gpuContext { instance, surface }
    Config* config
    PlatformInputPipe* platformInputPipe
    PtyFactory* ptyFactory
    std::string shadersDir
}
    │
    ▼
YettyContext {
    AppContext appContext                    // COPY
    YettyGpuContext gpuContext {
        AppGpuContext appGpuContext          // COPY
        adapter, device, queue, surfaceFormat
    }
    SharedBindGroup* sharedBindGroup         // owned by Yetty
}
    │
    ▼
TerminalScreenContext {
    YettyContext yettyContext                // COPY
    TerminalScreenGpuContext gpuContext {
        YettyGpuContext yettyGpuContext      // COPY
        GpuAllocator* allocator              // owned by TerminalScreen
        ShaderManager* shaderManager         // owned by TerminalScreen
    }
    Pty* pty                                 // owned by Terminal
}
```

## Ownership

- Contexts contain **copies** of parent contexts (by value)
- Contexts contain **pointers** to objects owned by that level
- The level that creates an object owns it and deletes it
- Pointers in contexts are valid as long as the owner lives
