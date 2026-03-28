#include <yetty/core/event-loop.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include "fd-pty-poll-source.hpp"
#include <ytrace/ytrace.hpp>
#include <uv.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <atomic>

namespace yetty {
namespace core {

struct EventTypeHash {
    std::size_t operator()(Event::Type t) const noexcept {
        return static_cast<std::size_t>(t);
    }
};

class EventLoopImpl;

struct PollHandle {
    uv_poll_t poll;
    int fd = -1;
    int events = UV_READABLE;
    EventLoopImpl* owner = nullptr;
    std::vector<EventListener*> listeners;
};

struct TimerHandle {
    uv_timer_t timer;
    int id = -1;
    Timeout timeout = 0;
    std::vector<EventListener*> listeners;
};

class EventLoopImpl : public EventLoop {
public:
    EventLoopImpl() {
        _loop = uv_default_loop();

        // Initialize render async handle
        _renderAsync.data = this;
        uv_async_init(_loop, &_renderAsync, onRenderAsync);

        // Initialize signal handlers for graceful shutdown
        _sigintHandle.data = this;
        _sigtermHandle.data = this;
        uv_signal_init(_loop, &_sigintHandle);
        uv_signal_init(_loop, &_sigtermHandle);
        uv_signal_start(&_sigintHandle, onSignal, SIGINT);
        uv_signal_start(&_sigtermHandle, onSignal, SIGTERM);
    }

    ~EventLoopImpl() override {
        uv_signal_stop(&_sigintHandle);
        uv_signal_stop(&_sigtermHandle);
        uv_close(reinterpret_cast<uv_handle_t*>(&_sigintHandle), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&_sigtermHandle), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&_renderAsync), nullptr);
    }

    static void onSignal(uv_signal_t* handle, int signum) {
        ydebug("EventLoop: received signal {} - stopping", signum);
        auto* self = static_cast<EventLoopImpl*>(handle->data);
        uv_stop(self->_loop);
    }

    Result<void> start() override {
        ydebug("EventLoop::start: running uv_default_loop");
        int r = uv_run(_loop, UV_RUN_DEFAULT);
        if (r < 0)
            return Err<void>(std::string("uv_run failed: ") + uv_strerror(r));
        return Ok();
    }

    Result<void> stop() override {
        ydebug("EventLoop::stop");
        uv_stop(_loop);
        return Ok();
    }

    Result<void> registerListener(Event::Type type, EventListener * listener, int priority = 0) override {
        if (!listener) return Err<void>("registerListener: null listener");
        auto& vec = _listeners[type];
        PrioritizedListener entry{listener, priority};
        auto insertPos = std::lower_bound(vec.begin(), vec.end(), entry,
            [](const PrioritizedListener& a, const PrioritizedListener& b) {
                return a.priority > b.priority;
            });
        vec.insert(insertPos, entry);
        return Ok();
    }

    Result<void> deregisterListener(Event::Type type, EventListener * listener) override {
        auto it = _listeners.find(type);
        if (it == _listeners.end()) return Ok();

        auto& vec = it->second;
        vec.erase(
            std::remove_if(vec.begin(), vec.end(),
                [&](const PrioritizedListener& pl) {
                    return pl.listener == listener;
                }),
            vec.end());
        return Ok();
    }

    Result<void> deregisterListener(EventListener * listener) override {
        for (auto& [type, vec] : _listeners) {
            vec.erase(
                std::remove_if(vec.begin(), vec.end(),
                    [&](const PrioritizedListener& pl) {
                        return pl.listener == listener;
                    }),
                vec.end());
        }
        return Ok();
    }

    Result<bool> dispatch(const Event& event) override {
        auto it = _listeners.find(event.type);
        if (it == _listeners.end()) {
            return Ok(false);
        }

        auto listeners = it->second;  // copy for safe iteration
        for (const auto& pl : listeners) {
            {
                auto result = pl.listener->onEvent(event);
                if (!result) {
                    return Err<bool>("Event handler failed", result);
                }
                if (*result) {
                    return Ok(true);  // consumed
                }
            }
        }
        return Ok(false);
    }

    Result<void> broadcast(const Event& event) override {
        auto listenersCopy = _listeners;
        for (auto& [type, vec] : listenersCopy) {
            for (const auto& pl : vec) {
                auto result = pl.listener->onEvent(event);
                if (!result) {
                    return Err<void>("Broadcast handler failed", result);
                }
            }
        }
        return Ok();
    }

    Result<PollId> createPoll() override {
        PollId id = _nextPollId++;
        auto ph = std::make_unique<PollHandle>();
        ph->owner = this;
        _polls[id] = std::move(ph);
        ydebug("EventLoop::createPoll: id={}", id);
        return Ok(id);
    }

    Result<void> configPoll(PollId id, int fd) override {
        auto it = _polls.find(id);
        if (it == _polls.end()) {
            return Err<void>("Poll not found");
        }

        auto& ph = it->second;
        ph->fd = fd;
#ifdef _WIN32
        int r = uv_poll_init_socket(_loop, &ph->poll, fd);
#else
        int r = uv_poll_init(_loop, &ph->poll, fd);
#endif
        if (r != 0) {
            yerror("EventLoop::configPoll: uv_poll_init failed for fd={}: {}", fd, uv_strerror(r));
            return Err<void>(std::string("uv_poll_init failed: ") + uv_strerror(r));
        }
        ph->poll.data = ph.get();
        ydebug("EventLoop::configPoll: id={} fd={} success", id, fd);
        return Ok();
    }

    Result<void> startPoll(PollId id, int events = POLL_READABLE) override {
        auto it = _polls.find(id);
        if (it == _polls.end()) {
            return Err<void>("Poll not found");
        }

        int uvEvents = 0;
        if (events & POLL_READABLE) uvEvents |= UV_READABLE;
        if (events & POLL_WRITABLE) uvEvents |= UV_WRITABLE;
        it->second->events = uvEvents;

        ydebug("EventLoop::startPoll: id={} fd={} events={}", id, it->second->fd, uvEvents);
        int r = uv_poll_start(&it->second->poll, uvEvents, onPollCallback);
        if (r != 0) {
            yerror("EventLoop::startPoll: uv_poll_start failed for fd={}: {}", it->second->fd, uv_strerror(r));
            return Err<void>(std::string("uv_poll_start failed: ") + uv_strerror(r));
        }
        return Ok();
    }

    Result<void> setPollEvents(PollId id, int events) override {
        auto it = _polls.find(id);
        if (it == _polls.end()) {
            return Err<void>("Poll not found");
        }

        int uvEvents = 0;
        if (events & POLL_READABLE) uvEvents |= UV_READABLE;
        if (events & POLL_WRITABLE) uvEvents |= UV_WRITABLE;

        if (it->second->events == uvEvents) {
            return Ok();
        }

        it->second->events = uvEvents;
        ydebug("EventLoop::setPollEvents: id={} fd={} events={}", id, it->second->fd, uvEvents);

        int r = uv_poll_start(&it->second->poll, uvEvents, onPollCallback);
        if (r != 0) {
            yerror("EventLoop::setPollEvents: uv_poll_start failed for fd={}: {}", it->second->fd, uv_strerror(r));
            return Err<void>(std::string("uv_poll_start failed: ") + uv_strerror(r));
        }
        return Ok();
    }

    Result<void> stopPoll(PollId id) override {
        auto it = _polls.find(id);
        if (it == _polls.end()) {
            return Err<void>("Poll not found");
        }

        uv_poll_stop(&it->second->poll);
        return Ok();
    }

    Result<void> destroyPoll(PollId id) override {
        auto it = _polls.find(id);
        if (it == _polls.end()) {
            return Err<void>("Poll not found");
        }

        uv_poll_stop(&it->second->poll);

        // Move to pending close set - libuv needs handle alive until close callback
        auto handle = std::move(it->second);
        _polls.erase(it);

        auto* rawPtr = handle.release();
        _pendingPollClose.insert(rawPtr);

        uv_close(reinterpret_cast<uv_handle_t*>(&rawPtr->poll), onPollCloseCallback);
        return Ok();
    }

    static void onPollCloseCallback(uv_handle_t* handle) {
        auto* ph = reinterpret_cast<PollHandle*>(handle);
        if (ph->owner) {
            ph->owner->_pendingPollClose.erase(ph);
        }
        delete ph;
    }

    Result<void> registerPollListener(PollId id, EventListener * listener) override {
        if (!listener) return Err<void>("registerPollListener: null listener");
        auto it = _polls.find(id);
        if (it == _polls.end()) {
            return Err<void>("Poll not found");
        }

        ydebug("EventLoop::registerPollListener: id={} fd={}", id, it->second->fd);
        it->second->listeners.push_back(listener);
        return Ok();
    }

    Result<TimerId> createTimer() override {
        TimerId id = _nextTimerId++;
        auto th = std::make_unique<TimerHandle>();
        th->id = id;
        uv_timer_init(_loop, &th->timer);
        th->timer.data = th.get();
        _timers[id] = std::move(th);
        ydebug("EventLoop::createTimer: id={}", id);
        return Ok(id);
    }

    Result<void> configTimer(TimerId id, Timeout timeoutMs) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }

        auto& th = it->second;
        th->timeout = timeoutMs;
        if (uv_is_active(reinterpret_cast<uv_handle_t*>(&th->timer))) {
            uv_timer_start(&th->timer, onTimerCallback, timeoutMs, timeoutMs);
        }
        ydebug("EventLoop::configTimer: id={} timeout={}", id, timeoutMs);
        return Ok();
    }

    Result<void> startTimer(TimerId id) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }

        auto& th = it->second;
        ydebug("EventLoop::startTimer: id={} timeout={}", id, th->timeout);
        uv_timer_start(&th->timer, onTimerCallback, th->timeout, th->timeout);
        return Ok();
    }

    Result<void> stopTimer(TimerId id) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }

        uv_timer_stop(&it->second->timer);
        return Ok();
    }

    Result<void> destroyTimer(TimerId id) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }

        uv_timer_stop(&it->second->timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&it->second->timer), nullptr);
        _timers.erase(it);
        return Ok();
    }

    Result<void> registerTimerListener(TimerId id, EventListener * listener) override {
        if (!listener) return Err<void>("registerTimerListener: null listener");
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }

        ydebug("EventLoop::registerTimerListener: id={}", id);
        it->second->listeners.push_back(listener);
        return Ok();
    }

    Result<PollId> createPtyPoll(PtyPollSource *source) override {
        auto *fdSource = static_cast<FdPtyPollSource *>(source);
        int fd = fdSource->fd();

        auto pollResult = createPoll();
        if (!pollResult) return pollResult;
        PollId id = *pollResult;

        if (auto res = configPoll(id, fd); !res) {
            destroyPoll(id);
            return Err<PollId>("createPtyPoll: configPoll failed", res);
        }

        return Ok(id);
    }

    void requestRender() override {
        _renderPending = true;
        uv_async_send(&_renderAsync);
    }

    Result<PollId> createPlatformInputPipePoll(PlatformInputPipe* pipe) override {
        _platformInputPipe = pipe;

        auto pollResult = createPoll();
        if (!pollResult) return pollResult;
        PollId id = *pollResult;

        if (auto res = configPoll(id, pipe->readFd()); !res) {
            destroyPoll(id);
            return Err<PollId>("createPlatformInputPipePoll: configPoll failed", res);
        }

        _platformInputPipePollId = id;
        pipe->setEventLoop(this);
        ydebug("EventLoop: created PlatformInputPipePoll id={} fd={}", id, pipe->readFd());
        return Ok(id);
    }

    Result<void> startPlatformInputPipePoll(PollId id) override {
        return startPoll(id);
    }

    Result<void> registerPlatformInputPipePollListener(PollId id, EventListener* listener) override {
        return registerPollListener(id, listener);
    }

private:
    static void onRenderAsync(uv_async_t* handle) {
        auto* self = static_cast<EventLoopImpl*>(handle->data);
        if (self->_renderPending.exchange(false)) {
            self->dispatch(Event::renderEvent());
        } else {
            ywarn("EventLoop::onRenderAsync: render signal but not pending");
        }
    }

    static void onPollCallback(uv_poll_t* handle, int status, int events) {
        auto* ph = static_cast<PollHandle*>(handle->data);
        ydebug("EventLoop::onPollCallback: fd={} status={} events={} listeners={}",
               ph->fd, status, events, ph->listeners.size());

        if (status < 0) {
            yerror("EventLoop::onPollCallback: error status={} for fd={}", status, ph->fd);
            return;
        }

        // Check if this is the PlatformInputPipe poll
        if (ph->owner->_platformInputPipe &&
            ph->fd == ph->owner->_platformInputPipe->readFd()) {
            if (events & UV_READABLE) {
                Event pipeEvent;
                while (ph->owner->_platformInputPipe->read(&pipeEvent, sizeof(pipeEvent)) == sizeof(pipeEvent)) {
                    ydebug("EventLoop: pipe event type={}", static_cast<int>(pipeEvent.type));
                    for (auto *listener : ph->listeners) {
                        listener->onEvent(pipeEvent);
                    }
                }
            } else {
                yerror("EventLoop: unexpected event {} on PlatformInputPipe fd={}", events, ph->fd);
            }
            return;
        }

        // Regular poll (PTY, etc.)
        if (events & UV_READABLE) {
            Event event;
            event.type = Event::Type::PollReadable;
            event.poll.fd = ph->fd;

            for (auto *listener : ph->listeners) {
                listener->onEvent(event);
            }
        }

        if (events & UV_WRITABLE) {
            Event event;
            event.type = Event::Type::PollWritable;
            event.poll.fd = ph->fd;

            for (auto *listener : ph->listeners) {
                listener->onEvent(event);
            }
        }
    }

    static void onTimerCallback(uv_timer_t* handle) {
        auto* th = static_cast<TimerHandle*>(handle->data);
        ydebug("EventLoop::onTimerCallback: timerId={}", th->id);

        Event event = Event::timerEvent(th->id);

        for (auto *listener : th->listeners) {
            listener->onEvent(event);
        }
    }

    struct PrioritizedListener {
        EventListener *listener;
        int priority;
    };

    std::unordered_map<Event::Type, std::vector<PrioritizedListener>, EventTypeHash> _listeners;

    uv_loop_t* _loop = nullptr;
    std::unordered_map<PollId, std::unique_ptr<PollHandle>> _polls;
    std::unordered_map<TimerId, std::unique_ptr<TimerHandle>> _timers;
    std::unordered_set<PollHandle*> _pendingPollClose;  // Handles awaiting async close
    PollId _nextPollId = 1;
    TimerId _nextTimerId = 1;

    // Platform input pipe - receives events from main thread
    PlatformInputPipe* _platformInputPipe = nullptr;
    PollId _platformInputPipePollId = -1;

    // Render async - for immediate re-render without waiting for timer
    uv_async_t _renderAsync;
    std::atomic<bool> _renderPending{false};

    // Signal handlers for graceful shutdown
    uv_signal_t _sigintHandle;
    uv_signal_t _sigtermHandle;
};

Result<EventLoop*> EventLoop::createImpl() noexcept {
    return Ok(static_cast<EventLoop*>(new EventLoopImpl()));
}

} // namespace core
} // namespace yetty
