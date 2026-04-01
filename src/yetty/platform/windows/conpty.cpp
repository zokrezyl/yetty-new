// Windows ConPTY implementation

#include <yetty/platform/pty.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/platform/pty-poll-source.hpp>
#include <ytrace/ytrace.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace yetty {

// WinPtyPollSource - wraps a HANDLE for polling
class WinPtyPollSource : public PtyPollSource {
public:
    explicit WinPtyPollSource(HANDLE h = INVALID_HANDLE_VALUE) : _handle(h) {}
    HANDLE handle() const { return _handle; }
private:
    HANDLE _handle;
};

class ConPty : public Pty {
public:
    ConPty() = default;
    ~ConPty() override { stop(); }

    Result<void> init(Config* config, uint32_t cols, uint32_t rows) {
        // TODO: Implement ConPTY initialization
        // - CreatePseudoConsole()
        // - Create input/output pipes
        // - Start child process with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
        _cols = cols;
        _rows = rows;
        yerror("ConPTY not yet implemented");
        return Err<void>("ConPTY not yet implemented");
    }

    size_t read(char* buf, size_t maxLen) override {
        // TODO: ReadFile from output pipe
        return 0;
    }

    void write(const char* data, size_t len) override {
        // TODO: WriteFile to input pipe
    }

    void resize(uint32_t cols, uint32_t rows) override {
        // TODO: ResizePseudoConsole()
        _cols = cols;
        _rows = rows;
    }

    void stop() override {
        if (!_running) return;
        _running = false;
        // TODO: ClosePseudoConsole(), close handles
    }

    PtyPollSource* pollSource() override { return &_pollSource; }

private:
    uint32_t _cols = 80;
    uint32_t _rows = 24;
    bool _running = false;
    WinPtyPollSource _pollSource;
    HANDLE _hPC = INVALID_HANDLE_VALUE;
    HANDLE _hPipeIn = INVALID_HANDLE_VALUE;
    HANDLE _hPipeOut = INVALID_HANDLE_VALUE;
};

class ConPtyFactory : public PtyFactory {
public:
    Result<Pty*> create(Config* config) override {
        auto* pty = new ConPty();
        uint32_t cols = static_cast<uint32_t>(config->get<int>("terminal/cols", 80));
        uint32_t rows = static_cast<uint32_t>(config->get<int>("terminal/rows", 24));
        auto result = pty->init(config, cols, rows);
        if (!result) {
            delete pty;
            return Err<Pty*>(result.error());
        }
        return Ok(static_cast<Pty*>(pty));
    }
};

Result<PtyFactory*> PtyFactory::create() {
    return Ok(static_cast<PtyFactory*>(new ConPtyFactory()));
}

} // namespace yetty
