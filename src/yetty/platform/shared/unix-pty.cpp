// Unix PTY I/O using forkpty() — shared by Linux and macOS

#include <yetty/platform/pty.hpp>
#include <yetty/platform/pty-factory.hpp>
#include "fd-pty-poll-source.hpp"
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

class UnixPty : public Pty {
public:
    UnixPty() = default;

    ~UnixPty() override {
        stop();
    }

    Result<void> init(Config* /*config*/) {
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

        _pollSource = FdPtyPollSource(_ptyMaster);
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

        if (_ptyMaster >= 0) {
            close(_ptyMaster);
            _ptyMaster = -1;
        }

        if (_childPid > 0) {
            kill(_childPid, SIGTERM);
            int status;
            waitpid(_childPid, &status, 0);
            _childPid = -1;
        }
    }

    PtyPollSource *pollSource() override {
        return &_pollSource;
    }

private:
    int _ptyMaster = -1;
    pid_t _childPid = -1;
    uint32_t _cols = 80;
    uint32_t _rows = 24;
    std::string _shell;
    std::string _command;
    bool _running = false;
    FdPtyPollSource _pollSource{-1};
};

class UnixPtyFactory : public PtyFactory {
public:
    explicit UnixPtyFactory(Config* config) : _config(config) {}

    Result<Pty *> createPty() override {
        auto *pty = new UnixPty();
        if (auto res = pty->init(_config); !res) {
            delete pty;
            return Err<Pty *>("Failed to create UnixPty", res);
        }
        return Ok(static_cast<Pty *>(pty));
    }

private:
    Config* _config;
};

Result<PtyFactory *> PtyFactory::createImpl(Config *config, void *) {
    return Ok(static_cast<PtyFactory *>(new UnixPtyFactory(config)));
}

} // namespace yetty
