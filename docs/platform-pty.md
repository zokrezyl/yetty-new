# Platform PTY Architecture

## Design Principle

All platforms model PTY I/O after the Unix fd pattern:

1. **Data buffer lives outside C++** (kernel on Unix, JS on WebASM)
2. **PollSource is notification-only** (like an fd - just tells you data is available)
3. **Pty::read() pulls data** from the external buffer when called

This keeps the architecture consistent across platforms and avoids duplicating buffers.

## Unix/Desktop (Linux, macOS)

### Components

```
Kernel PTY Buffer (owned by OS)
        ↑↓
    pty master fd
        ↑↓
┌─────────────────────────────────────────┐
│ UnixPty                                 │
│   _ptyMaster (fd)                       │
│   read()  → ::read(_ptyMaster, ...)     │
│   write() → ::write(_ptyMaster, ...)    │
│   pollSource() → FdPtyPollSource        │
└─────────────────────────────────────────┘
        │
┌─────────────────────────────────────────┐
│ FdPtyPollSource                         │
│   _fd (just holds the fd number)        │
│   fd() → returns _fd                    │
│   NO read/write methods                 │
└─────────────────────────────────────────┘
        │
┌─────────────────────────────────────────┐
│ EventLoop (libuv)                       │
│   createPtyPoll(source)                 │
│     → casts to FdPtyPollSource          │
│     → gets fd via source->fd()          │
│     → uv_poll_init(fd)                  │
│   When fd readable:                     │
│     → dispatches PollReadable event     │
└─────────────────────────────────────────┘
```

### Data Flow

**Shell output → Terminal screen:**
```
1. Shell writes to stdout
2. Data goes to kernel PTY buffer
3. Master fd becomes readable
4. libuv detects UV_READABLE
5. EventLoop dispatches PollReadable to listeners
6. Listener calls pty->read(buf, len)
7. UnixPty::read() calls ::read(_ptyMaster, buf, len)
8. Data copied from kernel buffer to buf
9. Listener feeds data to vterm_input_write()
```

**Keyboard → Shell:**
```
1. User types key
2. vterm produces escape sequence
3. vterm output callback calls pty->write(data, len)
4. UnixPty::write() calls ::write(_ptyMaster, data, len)
5. Data goes to kernel PTY buffer
6. Shell reads from stdin
```

## WebASM

### Architecture Challenge

WebASM has no file descriptors. JSLinux VM runs in an iframe and communicates via async postMessage. We cannot synchronously "read" from the VM.

**Solution:** Mirror the Unix model by keeping the buffer in JavaScript (parent window), with C++ calling into JS to read.

### Components

```
┌─────────────────────────────────────────┐
│ JSLinux VM (in iframe)                  │
│   produces output                       │
│     ↓                                   │
│   term_write(str)                       │
│     ↓                                   │
│   postMessage({type:'term-output',      │
│                data: str})              │
└─────────────────────────────────────────┘
        │ (postMessage carries data to parent)
        ↓
┌─────────────────────────────────────────┐
│ Parent Window JS                        │
│   var ptyBuffer = '';                   │
│                                         │
│   onmessage('term-output'):             │
│     ptyBuffer += e.data.data            │
│     Module._webpty_data_available(ptr)  │
│                                         │
│   pty_read_buffer(maxLen):              │
│     var chunk = ptyBuffer.substr(0,max) │
│     ptyBuffer = ptyBuffer.substr(max)   │
│     return chunk                        │
└─────────────────────────────────────────┘
        │
┌─────────────────────────────────────────┐
│ WebasmPty                               │
│   _ptyId                                │
│   read()  → EM_ASM: pty_read_buffer()   │
│   write() → postMessage to iframe       │
│   pollSource() → WebasmPtyPollSource    │
│                                         │
│   onDataAvailable() ← called by JS      │
│     → _pollSource.notify()              │
└─────────────────────────────────────────┘
        │
┌─────────────────────────────────────────┐
│ WebasmPtyPollSource                     │
│   _notifyCallback                       │
│   setNotifyCallback(cb)                 │
│   notify() → calls _notifyCallback()    │
│   NO read/write methods                 │
│   NO buffer                             │
└─────────────────────────────────────────┘
        │
┌─────────────────────────────────────────┐
│ EventLoop (emscripten)                  │
│   createPtyPoll(source)                 │
│     → casts to WebasmPtyPollSource      │
│     → sets callback to dispatch         │
│       PollReadable event                │
│   When notify() called:                 │
│     → callback fires                    │
│     → dispatches PollReadable event     │
└─────────────────────────────────────────┘
```

### Data Flow

**VM output → Terminal screen:**
```
1. JSLinux VM produces output, calls term_write(str)
2. iframe sends postMessage({type:'term-output', data:str})
3. Parent window message listener receives
4. Data appended to parent's JS buffer: ptyBuffer += data
5. JS calls Module._webpty_data_available(ptyPointer)
6. WebasmPty::onDataAvailable() calls _pollSource.notify()
7. PollSource callback fires (set by EventLoop)
8. EventLoop dispatches PollReadable to listeners
9. Listener calls pty->read(buf, len)
10. WebasmPty::read() calls pty_read_buffer(len) via EM_ASM
11. Data returned from parent's JS buffer to C++
12. Listener feeds data to vterm_input_write()
```

**Keyboard → VM:**
```
1. User types key
2. vterm produces escape sequence
3. vterm output callback calls pty->write(data, len)
4. WebasmPty::write() sends postMessage({type:'term-input', data})
5. iframe receives, calls console_write1() for each char
6. JSLinux VM receives input
```

## Key Design Points

1. **Buffer location:**
   - Unix: kernel (accessed via fd)
   - WebASM: parent window JS (accessed via EM_ASM)

2. **PollSource responsibility:**
   - Unix: holds fd, EventLoop polls it
   - WebASM: holds callback, JS calls notify() when data arrives

3. **Pty::read() behavior:**
   - Unix: synchronous read from kernel via fd
   - WebASM: synchronous read from JS via EM_ASM

4. **No buffer in C++:**
   - Avoids double-buffering
   - Matches Unix model where kernel owns the buffer
   - PollSource never touches data, only signals availability

## Terminal Integration

Terminal must set up PTY polling:

```cpp
// In Terminal::init()
auto ptyPollId = _eventLoop->createPtyPoll(_pty->pollSource());
_eventLoop->registerPollListener(ptyPollId, _screen);
_eventLoop->startPoll(ptyPollId);
```

TerminalScreen must handle PollReadable:

```cpp
Result<bool> TerminalScreen::onEvent(const Event& event) {
    if (event.type == Event::Type::PollReadable) {
        char buf[4096];
        size_t n = _pty->read(buf, sizeof(buf));
        if (n > 0) {
            vterm_input_write(_vterm, buf, n);
        }
        return Ok(true);
    }
    // ... other events
}
```
