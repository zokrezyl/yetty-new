#pragma once

#include <yetty/core/result.hpp>
#include <cstddef>

namespace yetty {
namespace core {

class EventLoop;
class EventListener;

class PlatformInputPipe {
public:
    virtual ~PlatformInputPipe() = default;

    // Write event data to pipe (called from main/platform thread)
    virtual void write(const void* data, size_t size) = 0;

    // Read event data from pipe (called from consumer thread/callback)
    virtual size_t read(void* data, size_t maxSize) = 0;

    // Get read fd for polling (desktop only, returns -1 on webasm)
    virtual int readFd() const = 0;

    // Called by EventLoop to connect (webasm needs these for dispatch)
    virtual void setEventLoop(EventLoop* loop) = 0;
    virtual void setListener(EventListener* listener) = 0;

    // Platform-specific factory
    static Result<PlatformInputPipe*> create();
};

} // namespace core
} // namespace yetty
