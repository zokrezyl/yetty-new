#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/core/event-listener.hpp>
#include <yetty/core/event.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>
#include <vector>
#include <cstring>

namespace yetty {
namespace core {

class WebasmPlatformInputPipe : public PlatformInputPipe {
public:
    WebasmPlatformInputPipe() = default;
    ~WebasmPlatformInputPipe() override = default;

    void write(const void* data, size_t size) override {
        if (size == 0) return;

        const char* bytes = static_cast<const char*>(data);
        _buffer.insert(_buffer.end(), bytes, bytes + size);

        // Schedule async callback to notify listener
        if (!_callbackPending && _listener) {
            _callbackPending = true;
            emscripten_async_call(onDataAvailable, this, 0);
        }
    }

    size_t read(void* data, size_t maxSize) override {
        if (_buffer.empty() || maxSize == 0) return 0;

        size_t toRead = std::min(maxSize, _buffer.size());
        std::memcpy(data, _buffer.data(), toRead);
        _buffer.erase(_buffer.begin(), _buffer.begin() + toRead);

        return toRead;
    }

    int readFd() const override {
        return -1;  // No fd on webasm
    }

    void setEventLoop(EventLoop* loop) override {
        _eventLoop = loop;
    }

    void setListener(EventListener* listener) override {
        _listener = listener;
    }

private:
    static void onDataAvailable(void* arg) {
        auto* self = static_cast<WebasmPlatformInputPipe*>(arg);
        self->_callbackPending = false;

        if (!self->_eventLoop) {
            ywarn("PlatformInputPipe::onDataAvailable: no EventLoop");
            return;
        }

        // Read Event structs from buffer and dispatch (mirrors libuv-event-loop.cpp)
        Event pipeEvent;
        while (self->_buffer.size() >= sizeof(Event)) {
            std::memcpy(&pipeEvent, self->_buffer.data(), sizeof(Event));
            self->_buffer.erase(self->_buffer.begin(), self->_buffer.begin() + sizeof(Event));
            ydebug("PlatformInputPipe: dispatching event type={}", static_cast<int>(pipeEvent.type));
            self->_eventLoop->dispatch(pipeEvent);
        }
    }

    EventLoop* _eventLoop = nullptr;
    EventListener* _listener = nullptr;
    std::vector<char> _buffer;
    bool _callbackPending = false;
};

Result<PlatformInputPipe*> PlatformInputPipe::create() {
    return Ok(static_cast<PlatformInputPipe*>(new WebasmPlatformInputPipe()));
}

} // namespace core
} // namespace yetty
