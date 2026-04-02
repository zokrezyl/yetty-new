# Platform Input Pipe

## Overview

PlatformInputPipe transports `Event` structs from platform code (input callbacks) to the EventLoop. It is a dumb byte transport with a notification mechanism.

## Responsibilities

### Pipe
- Accept bytes via `write()`
- Return bytes via `read()`
- Notify EventLoop when data is available

The pipe does NOT interpret Event structs. It does NOT dispatch events. It is just bytes in, bytes out, plus notification.

### EventLoop
- Receives pipe pointer at creation time
- If pipe is non-null:
  - Creates poll for the pipe (fd-based on Linux, callback-based on WebASM)
  - Starts the poll
  - On notification: reads `Event` structs from pipe, calls `dispatch(event)` for each

### Listeners
- Register for specific event types: `KeyDown`, `KeyUp`, `Char`, `MouseDown`, `MouseUp`, `MouseMove`, `Scroll`, `Resize`, etc.
- Receive events via `onEvent()` when EventLoop dispatches matching type

## Platform Differences

| Platform | Notification Mechanism |
|----------|------------------------|
| Linux/macOS | libuv polls read fd |
| WebASM | `emscripten_async_call` triggers callback |
| Windows | libuv polls read handle |

The notification mechanism differs, but the flow is identical:

```
Platform Input Callback
    |
    v
pipe->write(Event)
    |
    v
[notification triggers]
    |
    v
EventLoop receives notification
    |
    v
EventLoop calls pipe->read()
    |
    v
EventLoop calls dispatch(event) — by event type
    |
    v
Listeners registered for that event type receive it
```

## Interface

```cpp
class PlatformInputPipe {
    void write(const void* data, size_t size);
    size_t read(void* data, size_t maxSize);
    int readFd() const;  // -1 on platforms without fd
    void setEventLoop(EventLoop* loop);
};
```

## Usage

```cpp
// Create EventLoop with pipe
// EventLoop internally: creates poll, starts poll, handles notifications
auto eventLoop = EventLoop::create(pipe);

// Register listeners for specific event types
eventLoop->registerListener(Event::Type::KeyDown, terminalScreen);
eventLoop->registerListener(Event::Type::KeyUp, terminalScreen);
eventLoop->registerListener(Event::Type::Char, terminalScreen);
eventLoop->registerListener(Event::Type::MouseDown, terminalScreen);
eventLoop->registerListener(Event::Type::MouseUp, terminalScreen);
eventLoop->registerListener(Event::Type::MouseMove, terminalScreen);
eventLoop->registerListener(Event::Type::Scroll, terminalScreen);
eventLoop->registerListener(Event::Type::Resize, terminalScreen);

// Start event loop
eventLoop->start();
```

## EventLoop Internal Behavior

When created with a non-null pipe:

1. **Linux/macOS/Windows**: Create uv_poll for `pipe->readFd()`, start polling
2. **WebASM**: Call `pipe->setEventLoop(this)` so pipe can trigger callback

On pipe notification:
```cpp
Event event;
while (_pipe->read(&event, sizeof(event)) == sizeof(event)) {
    dispatch(event);
}
```
