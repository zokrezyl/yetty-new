#include <yetty/core/event-queue.hpp>
#include <yetty/core/event-loop.hpp>
#include <ytrace/ytrace.hpp>
#include <queue>
#include <mutex>

namespace yetty {
namespace core {

#if defined(YETTY_WEB)

class EventQueueImpl : public EventQueue {
public:
    EventQueueImpl() = default;
    ~EventQueueImpl() override = default;

    void setEventLoop(EventLoop* loop) override {
        _eventLoop = loop;
    }

    void push(const Event& event) override {
        _queue.push(event);
        if (_eventLoop) {
            _eventLoop->onRawEventQueueEvent();
        }
    }

    void drain() override {
        if (!_eventLoop) return;

        while (!_queue.empty()) {
            (void)_eventLoop->dispatch(_queue.front());
            _queue.pop();
        }
    }

private:
    EventLoop* _eventLoop = nullptr;
    std::queue<Event> _queue;
};

#else

class EventQueueImpl : public EventQueue {
public:
    EventQueueImpl() = default;
    ~EventQueueImpl() override = default;

    void setEventLoop(EventLoop* loop) override {
        _eventLoop = loop;
    }

    void push(const Event& event) override {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _queue.push(event);
        }

        if (_eventLoop) {
            _eventLoop->onRawEventQueueEvent();
        } else {
            yerror("EventQueue::push: no EventLoop!");
        }
    }

    void drain() override {
        if (!_eventLoop) {
            yerror("EventQueue::drain: EventLoop not available!");
            return;
        }

        std::queue<Event> events;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::swap(events, _queue);
        }

        while (!events.empty()) {
            auto& e = events.front();
            if (e.type == Event::Type::Char) {
                ydebug("EventQueue::drain: dispatching Char event cp={}", e.chr.codepoint);
            }
            (void)_eventLoop->dispatch(e);
            events.pop();
        }
    }

private:
    EventLoop* _eventLoop = nullptr;
    std::mutex _mutex;
    std::queue<Event> _queue;
};

#endif // YETTY_WEB

Result<EventQueue*> EventQueue::createImpl() noexcept {
    return Ok(static_cast<EventQueue*>(new EventQueueImpl()));
}

} // namespace core
} // namespace yetty
