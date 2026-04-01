#include <yetty/core/event-loop.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include "webasm-pty-poll-source.hpp"
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace yetty {
namespace core {

struct EventTypeHash {
    std::size_t operator()(Event::Type t) const noexcept {
        return static_cast<std::size_t>(t);
    }
};

class EventLoopImpl : public EventLoop {
public:
    EventLoopImpl() = default;
    ~EventLoopImpl() override = default;

    Result<void> start() override {
        ydebug("EventLoop::start (webasm) - setting up emscripten main loop");
        _running = true;
        // Set up browser main loop - runs at ~60fps via requestAnimationFrame
        // The callback fires timer events for registered timers
        emscripten_set_main_loop_arg(mainLoopCallback, this, 0, true);
        // Note: emscripten_set_main_loop with simulateInfiniteLoop=true doesn't return
        return Ok();
    }

    Result<void> stop() override {
        ydebug("EventLoop::stop (webasm)");
        _running = false;
        emscripten_cancel_main_loop();
        return Ok();
    }

    Result<void> registerListener(Event::Type type, EventListener* listener, int priority = 0) override {
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

    Result<void> deregisterListener(Event::Type type, EventListener* listener) override {
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

    Result<void> deregisterListener(EventListener* listener) override {
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
            auto result = pl.listener->onEvent(event);
            if (!result) {
                return Err<bool>("Event handler failed", result);
            }
            if (*result) {
                return Ok(true);  // consumed
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

    // Poll - callback-based on webasm (no fd polling)
    Result<PollId> createPoll() override {
        PollId id = _nextPollId++;
        _pollListeners[id] = {};
        return Ok(id);
    }
    Result<void> configPoll(PollId, int) override {
        return Ok();  // No-op on webasm
    }
    Result<void> startPoll(PollId, int) override {
        return Ok();  // No-op on webasm - callbacks handle notification
    }
    Result<void> setPollEvents(PollId, int) override {
        return Ok();  // No-op on webasm
    }
    Result<void> stopPoll(PollId) override {
        return Ok();  // No-op on webasm
    }
    Result<void> destroyPoll(PollId id) override {
        _pollListeners.erase(id);
        return Ok();
    }
    Result<void> registerPollListener(PollId id, EventListener* listener) override {
        _pollListeners[id].push_back(listener);
        return Ok();
    }

    Result<PollId> createPtyPoll(PtyPollSource* source) override {
        auto pollResult = createPoll();
        if (!pollResult) return pollResult;
        PollId id = *pollResult;

        // Cast to WebasmPtyPollSource and set callback to dispatch PollReadable
        auto* webasmSource = static_cast<WebasmPtyPollSource*>(source);
        webasmSource->setNotifyCallback([this, id]() {
            // Dispatch PollReadable to registered listeners
            Event event;
            event.type = Event::Type::PollReadable;
            event.poll.fd = id;  // Use poll id as fake fd
            for (auto* listener : _pollListeners[id]) {
                listener->onEvent(event);
            }
        });

        return Ok(id);
    }

    Result<PollId> createPlatformInputPipePoll(PlatformInputPipe* pipe) override {
        auto pollResult = createPoll();
        if (!pollResult) return pollResult;
        PollId id = *pollResult;
        _platformInputPipe = pipe;
        _platformInputPipePollId = id;
        pipe->setEventLoop(this);
        return Ok(id);
    }

    Result<void> startPlatformInputPipePoll(PollId /*id*/) override {
        return Ok();  // No-op on webasm - pipe triggers listener directly
    }

    Result<void> registerPlatformInputPipePollListener(PollId id, EventListener* listener) override {
        if (_platformInputPipe) {
            _platformInputPipe->setListener(listener);
        }
        _pollListeners[id].push_back(listener);
        return Ok();
    }

    // Timer
    Result<TimerId> createTimer() override {
        TimerId id = _nextTimerId++;
        _timers[id] = TimerData{};
        ydebug("EventLoop::createTimer (webasm): id={}", id);
        return Ok(id);
    }

    Result<void> configTimer(TimerId id, Timeout timeoutMs) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }
        it->second.timeout = timeoutMs;
        ydebug("EventLoop::configTimer (webasm): id={} timeout={}", id, timeoutMs);
        return Ok();
    }

    Result<void> startTimer(TimerId id) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }
        it->second.running = true;
        it->second.lastFire = emscripten_get_now();
        ydebug("EventLoop::startTimer (webasm): id={}", id);
        return Ok();
    }

    Result<void> stopTimer(TimerId id) override {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }
        it->second.running = false;
        ydebug("EventLoop::stopTimer (webasm): id={}", id);
        return Ok();
    }

    Result<void> destroyTimer(TimerId id) override {
        _timers.erase(id);
        ydebug("EventLoop::destroyTimer (webasm): id={}", id);
        return Ok();
    }

    Result<void> registerTimerListener(TimerId id, EventListener* listener) override {
        if (!listener) return Err<void>("registerTimerListener: null listener");
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return Err<void>("Timer not found");
        }
        it->second.listeners.push_back(listener);
        ydebug("EventLoop::registerTimerListener (webasm): id={}", id);
        return Ok();
    }

    void requestRender() override {
        dispatch(Event::renderEvent());
    }

private:
    static void mainLoopCallback(void* arg) {
        auto* self = static_cast<EventLoopImpl*>(arg);
        self->tick();
    }

    void tick() {
        if (!_running) return;

        double now = emscripten_get_now();

        // Fire timers that are due
        for (auto& [id, td] : _timers) {
            if (!td.running) continue;

            double elapsed = now - td.lastFire;
            if (elapsed >= td.timeout) {
                td.lastFire = now;

                Event event = Event::timerEvent(id);
                for (auto* listener : td.listeners) {
                    listener->onEvent(event);
                }
            }
        }
    }

    struct PrioritizedListener {
        EventListener* listener;
        int priority;
    };

    struct TimerData {
        Timeout timeout = 0;
        bool running = false;
        double lastFire = 0;
        std::vector<EventListener*> listeners;
    };

    std::unordered_map<Event::Type, std::vector<PrioritizedListener>, EventTypeHash> _listeners;
    std::unordered_map<TimerId, TimerData> _timers;
    std::unordered_map<PollId, std::vector<EventListener*>> _pollListeners;
    TimerId _nextTimerId = 1;
    PollId _nextPollId = 1;
    bool _running = false;
    PlatformInputPipe* _platformInputPipe = nullptr;
    PollId _platformInputPipePollId = -1;
};

// Factory
Result<EventLoop*> EventLoop::createImpl() noexcept {
    return Ok(static_cast<EventLoop*>(new EventLoopImpl()));
}

} // namespace core
} // namespace yetty
