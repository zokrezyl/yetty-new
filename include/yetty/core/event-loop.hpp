#pragma once

#include <yetty/core/event-listener.hpp>
#include <yetty/core/event.hpp>
#include <yetty/core/factory-object.hpp>
#include <yetty/platform/pty-poll-source.hpp>

namespace yetty {
namespace core {

using PollId = int;
using TimerId = int;
using Timeout = int;

class PlatformInputPipe;

class EventLoop : public FactoryObject<EventLoop> {
public:
  using Ptr = std::shared_ptr<EventLoop>;

  static Result<EventLoop*> createImpl() noexcept;

  virtual ~EventLoop() = default;

  // Start the event loop (blocking)
  virtual Result<void> start() = 0;

  // Stop the event loop
  virtual Result<void> stop() = 0;

  // Event listener registration by type
  // priority: higher value = called first (default 0)
  virtual Result<void> registerListener(Event::Type type,
                                        EventListener * listener,
                                        int priority = 0) = 0;
  virtual Result<void> deregisterListener(Event::Type type,
                                          EventListener * listener) = 0;
  virtual Result<void> deregisterListener(EventListener * listener) = 0;

  virtual Result<bool> dispatch(const Event &event) = 0;
  virtual Result<void> broadcast(const Event &event) = 0;

  // Poll (file descriptor) management
  enum PollEvents : int {
    POLL_READABLE = 1,
    POLL_WRITABLE = 2,
  };
  virtual Result<PollId> createPoll() = 0;
  virtual Result<void> configPoll(PollId id, int fd) = 0;
  virtual Result<void> startPoll(PollId id, int events = POLL_READABLE) = 0;
  virtual Result<void> setPollEvents(PollId id, int events) = 0;
  virtual Result<void> stopPoll(PollId id) = 0;
  virtual Result<void> destroyPoll(PollId id) = 0;
  virtual Result<void> registerPollListener(PollId id,
                                            EventListener * listener) = 0;

  // PTY poll — platform-independent PTY data monitoring
  //
  // createPtyPoll: takes an opaque PtyPollSource from Pty::pollSource().
  // The platform-specific EventLoop static_casts it to the concrete type
  // it knows (FdPtyPollSource on Unix, WebasmPtyPollSource on webasm)
  // and sets up platform-specific monitoring:
  //   - Unix: uv_poll_init on the fd inside FdPtyPollSource
  //   - Webasm: registers JS postMessage listener via WebasmPtyPollSource
  //
  // Returns a PollId. Use registerPollListener to add listeners, then
  // startPoll to begin monitoring. When data arrives, EventLoop dispatches
  // PollReadable to all registered listeners.
  virtual Result<PollId> createPtyPoll(PtyPollSource *source) = 0;

  // Timer management
  virtual Result<TimerId> createTimer() = 0;
  virtual Result<void> configTimer(TimerId id, Timeout timeoutMs) = 0;
  virtual Result<void> startTimer(TimerId id) = 0;
  virtual Result<void> stopTimer(TimerId id) = 0;
  virtual Result<void> destroyTimer(TimerId id) = 0;
  virtual Result<void> registerTimerListener(TimerId id,
                                             EventListener * listener) = 0;

  // Async render - dispatch Render event asynchronously
  // This allows immediate re-render without waiting for the frame timer
  virtual void requestRender() = 0;

  // Platform input pipe poll - receives events from main/platform thread
  // Desktop: uses uv_poll on pipe->readFd()
  // Webasm: pipe triggers listener via emscripten_async_call
  virtual Result<PollId> createPlatformInputPipePoll(PlatformInputPipe* pipe) = 0;
  virtual Result<void> startPlatformInputPipePoll(PollId id) = 0;
  virtual Result<void> registerPlatformInputPipePollListener(PollId id, EventListener* listener) = 0;

protected:
  EventLoop() = default;
};

} // namespace core
} // namespace yetty
