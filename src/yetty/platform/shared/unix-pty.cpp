// Unix PTY I/O using forkpty() — shared by Linux and macOS

#include <yetty/platform/pty.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/core/event-loop.hpp>
#include <ytrace/ytrace.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <pty.h>
#include <cstring>
#include <vector>

namespace yetty {

namespace {

std::vector<std::string> parseCommand(const std::string& cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escape = false;

    for (char c : cmd) {
        if (escape) {
            current += c;
            escape = false;
            continue;
        }
        if (c == '\\' && !inSingleQuote) {
            escape = true;
            continue;
        }
        if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            continue;
        }
        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }
        if (c == ' ' && !inSingleQuote && !inDoubleQuote) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
            continue;
        }
        current += c;
    }
    if (!current.empty()) {
        args.push_back(current);
    }
    return args;
}

} // anonymous namespace

class UnixPty : public Pty, public core::EventListener {
public:
    UnixPty() = default;

    ~UnixPty() override {
        stop();
    }

    Result<void> init(Config* config) {
        // TODO: read shell/command/cols/rows from Config
        _shell = "/bin/bash";
        _cols = 80;
        _rows = 24;

        struct winsize ws = {
            static_cast<unsigned short>(_rows),
            static_cast<unsigned short>(_cols),
            0, 0
        };

        _childPid = forkpty(&_ptyMaster, nullptr, nullptr, &ws);

        if (_childPid < 0) {
            return Err<void>(std::string("forkpty failed: ") + strerror(errno));
        }

        if (_childPid == 0) {
            // Child process
            for (int fd = 3; fd < 1024; fd++) {
                close(fd);
            }

            if (!_command.empty()) {
                auto args = parseCommand(_command);
                if (args.empty()) {
                    _exit(1);
                }
                std::vector<char*> argv;
                for (auto& arg : args) {
                    argv.push_back(const_cast<char*>(arg.c_str()));
                }
                argv.push_back(nullptr);
                execvp(argv[0], argv.data());
                _exit(1);
            } else {
                execl(_shell.c_str(), _shell.c_str(), nullptr);
                _exit(1);
            }
        }

        // Parent process - set non-blocking
        int flags = fcntl(_ptyMaster, F_GETFL, 0);
        fcntl(_ptyMaster, F_SETFL, flags | O_NONBLOCK);

        // Set up event loop polling
        auto loopResult = core::EventLoop::instance();
        if (!loopResult) {
            return Err<void>("No event loop available");
        }
        auto loop = *loopResult;

        auto pollResult = loop->createPoll();
        if (!pollResult) {
            return Err<void>("Failed to create poll", pollResult);
        }
        _pollId = *pollResult;

        if (auto res = loop->configPoll(_pollId, _ptyMaster); !res) {
            return Err<void>("Failed to configure poll", res);
        }

        if (auto res = loop->registerPollListener(_pollId, sharedAs<core::EventListener>()); !res) {
            return Err<void>("Failed to register poll listener", res);
        }

        if (auto res = loop->startPoll(_pollId); !res) {
            return Err<void>("Failed to start poll", res);
        }

        _running = true;
        ydebug("UnixPty: Started fd={}, PID={}, shell={}", _ptyMaster, _childPid, _shell);
        return Ok();
    }

    size_t read(char* buf, size_t maxLen) override {
        if (_ptyMaster < 0) return 0;

        ssize_t n = ::read(_ptyMaster, buf, maxLen);
        if (n > 0) {
            return static_cast<size_t>(n);
        }
        return 0;
    }

    void write(const char* data, size_t len) override {
        if (_ptyMaster >= 0 && len > 0) {
            ssize_t written = ::write(_ptyMaster, data, len);
            (void)written;
        }
    }

    void resize(uint32_t cols, uint32_t rows) override {
        _cols = cols;
        _rows = rows;

        if (_ptyMaster >= 0) {
            struct winsize ws = {
                static_cast<unsigned short>(rows),
                static_cast<unsigned short>(cols),
                0, 0
            };
            ioctl(_ptyMaster, TIOCSWINSZ, &ws);
        }
    }

    bool isRunning() const override {
        return _running;
    }

    void stop() override {
        if (!_running) return;
        _running = false;

        ydebug("UnixPty: Stopping");

        if (_pollId >= 0) {
            if (auto loopResult = core::EventLoop::instance(); loopResult) {
                auto loop = *loopResult;
                if (auto self = weak_from_this().lock()) {
                    loop->deregisterListener(sharedAs<core::EventListener>());
                }
                loop->destroyPoll(_pollId);
            }
            _pollId = -1;
        }

        if (_ptyMaster >= 0) {
            close(_ptyMaster);
            _ptyMaster = -1;
        }

        if (_childPid > 0) {
            kill(_childPid, SIGTERM);
            int status;
            waitpid(_childPid, &status, 0);
            if (_exitCallback) {
                _exitCallback(WEXITSTATUS(status));
            }
            _childPid = -1;
        }
    }

    void setDataAvailableCallback(DataAvailableCallback cb) override {
        _dataAvailableCallback = std::move(cb);
    }

    void setExitCallback(ExitCallback cb) override {
        _exitCallback = std::move(cb);
    }

    Result<bool> onEvent(const core::Event& event) override {
        if (event.type == core::Event::Type::PollReadable &&
            event.poll.fd == _ptyMaster) {

            int status;
            if (waitpid(_childPid, &status, WNOHANG) > 0) {
                _running = false;
                if (_exitCallback) {
                    _exitCallback(WEXITSTATUS(status));
                }
                return Ok(true);
            }

            if (_dataAvailableCallback) {
                _dataAvailableCallback();
            }
            return Ok(true);
        }
        return Ok(false);
    }

private:
    int _ptyMaster = -1;
    pid_t _childPid = -1;
    core::PollId _pollId = -1;
    uint32_t _cols = 80;
    uint32_t _rows = 24;
    std::string _shell;
    std::string _command;
    bool _running = false;
    DataAvailableCallback _dataAvailableCallback;
    ExitCallback _exitCallback;
};

class UnixPtyFactory : public PtyFactory {
public:
    Result<Pty::Ptr> create(Config* config, void* /*osSpecific*/) override {
        auto pty = std::make_shared<UnixPty>();
        if (auto res = pty->init(config); !res) {
            return Err<Pty::Ptr>("Failed to create UnixPty", res);
        }
        return Ok<Pty::Ptr>(pty);
    }
};

PtyFactory::Ptr createPtyFactory() {
    return std::make_shared<UnixPtyFactory>();
}

} // namespace yetty
