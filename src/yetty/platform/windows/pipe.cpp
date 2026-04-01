// Windows PlatformInputPipe - uses Windows named pipes

#include <yetty/core/platform-input-pipe.hpp>
#include <ytrace/ytrace.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace yetty {
namespace core {

class WinPlatformInputPipe : public PlatformInputPipe {
public:
    WinPlatformInputPipe() = default;

    ~WinPlatformInputPipe() override {
        if (_hRead != INVALID_HANDLE_VALUE) CloseHandle(_hRead);
        if (_hWrite != INVALID_HANDLE_VALUE) CloseHandle(_hWrite);
    }

    Result<void> init() {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        if (!CreatePipe(&_hRead, &_hWrite, &sa, 0)) {
            return Err<void>("CreatePipe failed: " + std::to_string(GetLastError()));
        }

        // Make read handle non-blocking via overlapped I/O or use async read
        ydebug("WinPlatformInputPipe: created");
        return Ok();
    }

    void write(const void* data, size_t size) override {
        if (_hWrite == INVALID_HANDLE_VALUE || size == 0) return;
        DWORD written = 0;
        WriteFile(_hWrite, data, static_cast<DWORD>(size), &written, nullptr);
    }

    size_t read(void* data, size_t maxSize) override {
        if (_hRead == INVALID_HANDLE_VALUE || maxSize == 0) return 0;
        DWORD available = 0;
        if (!PeekNamedPipe(_hRead, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            return 0;
        }
        DWORD bytesRead = 0;
        ReadFile(_hRead, data, static_cast<DWORD>(std::min(maxSize, static_cast<size_t>(available))), &bytesRead, nullptr);
        return bytesRead;
    }

    int readFd() const override {
        return -1;  // No fd on Windows
    }

    void setEventLoop(EventLoop* loop) override {
        _eventLoop = loop;
    }

    void setListener(EventListener* listener) override {
        _listener = listener;
    }

private:
    HANDLE _hRead = INVALID_HANDLE_VALUE;
    HANDLE _hWrite = INVALID_HANDLE_VALUE;
    EventLoop* _eventLoop = nullptr;
    EventListener* _listener = nullptr;
};

Result<PlatformInputPipe*> PlatformInputPipe::create() {
    auto* pipe = new WinPlatformInputPipe();
    auto result = pipe->init();
    if (!result) {
        delete pipe;
        return Err<PlatformInputPipe*>(result.error());
    }
    return Ok(static_cast<PlatformInputPipe*>(pipe));
}

} // namespace core
} // namespace yetty
