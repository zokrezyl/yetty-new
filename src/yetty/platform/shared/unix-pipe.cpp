#include <yetty/core/platform-input-pipe.hpp>
#include <ytrace/ytrace.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace yetty {
namespace core {

class UnixPlatformInputPipe : public PlatformInputPipe {
public:
    UnixPlatformInputPipe() = default;

    ~UnixPlatformInputPipe() override {
        if (_readFd >= 0) ::close(_readFd);
        if (_writeFd >= 0) ::close(_writeFd);
    }

    Result<void> init() {
        int fds[2];
        if (::pipe(fds) != 0) {
            return Err<void>("pipe() failed: " + std::string(strerror(errno)));
        }

        _readFd = fds[0];
        _writeFd = fds[1];

        // Set read end non-blocking
        int flags = fcntl(_readFd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(_readFd, F_SETFL, flags | O_NONBLOCK);
        }

        ydebug("UnixPlatformInputPipe: created readFd={} writeFd={}", _readFd, _writeFd);
        return Ok();
    }

    void write(const void* data, size_t size) override {
        if (_writeFd < 0 || size == 0) return;

        ssize_t written = ::write(_writeFd, data, size);
        if (written < 0) {
            yerror("UnixPlatformInputPipe::write failed: {}", strerror(errno));
        }
    }

    size_t read(void* data, size_t maxSize) override {
        if (_readFd < 0 || maxSize == 0) return 0;

        ssize_t bytesRead = ::read(_readFd, data, maxSize);
        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // No data available
            }
            yerror("UnixPlatformInputPipe::read failed: {}", strerror(errno));
            return 0;
        }

        return static_cast<size_t>(bytesRead);
    }

    int readFd() const override {
        return _readFd;
    }

    void setEventLoop(EventLoop* /*loop*/) override {
        // No-op on desktop - EventLoop polls the fd directly
    }

    void setListener(EventListener* /*listener*/) override {
        // No-op on desktop - EventLoop uses poll infrastructure
    }

private:
    int _readFd = -1;
    int _writeFd = -1;
};

Result<PlatformInputPipe*> PlatformInputPipe::create() {
    auto* pipe = new UnixPlatformInputPipe();
    if (auto res = pipe->init(); !res) {
        delete pipe;
        return Err<PlatformInputPipe*>("UnixPlatformInputPipe init failed", res);
    }
    return Ok(static_cast<PlatformInputPipe*>(pipe));
}

} // namespace core
} // namespace yetty
