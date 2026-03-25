#pragma once

#include <yetty/core/event.hpp>
#include <yetty/core/factory-object.hpp>

namespace yetty {
namespace core {

class EventLoop;

class EventQueue : public FactoryObject<EventQueue> {
public:
  using Ptr = std::shared_ptr<EventQueue>;

  static Result<EventQueue*> createImpl() noexcept;

  virtual ~EventQueue() = default;

  // Called by EventLoop::connectEventQueue() to set the event loop pointer
  virtual void setEventLoop(EventLoop* loop) = 0;

  // Push event to queue (thread-safe, can be called from any thread)
  // Wakes up main thread to process the event
  virtual void push(const Event &event) = 0;

  // Drain queued events (web-specific, called from main loop)
  // On native builds with libuv, this is a no-op (events are drained via async
  // callback)
  virtual void drain() {}

protected:
  EventQueue() = default;
};

} // namespace core
} // namespace yetty
