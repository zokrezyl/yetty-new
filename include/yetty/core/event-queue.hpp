#pragma once

#include <yetty/core/event.hpp>
#include <yetty/core/factory-object.hpp>

namespace yetty {
namespace core {

// EventQueue - thread-safe queue for cross-thread event delivery
//
// Use this when posting events from non-main threads (e.g., GPU callbacks).
// Main thread code should continue using EventLoop dispatch() directly.
//
class EventQueue : public FactoryObject<EventQueue> {
public:
  using Ptr = std::shared_ptr<EventQueue>;

  static Result<Ptr> createImpl() noexcept;

  virtual ~EventQueue() = default;

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
