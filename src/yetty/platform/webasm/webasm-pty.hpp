#pragma once

#include <yetty/platform/pty.hpp>
#include "webasm-pty-poll-source.hpp"
#include <cstdint>

namespace yetty {

class Config;

// WebasmPty - PTY implementation for WebASM using JSLinux iframe
//
// Buffer lives in parent window JS (like kernel buffer on Unix).
// read() calls into JS to pull data from buffer via EM_ASM.
// write() sends postMessage to iframe.
//
class WebasmPty : public Pty {
public:
    WebasmPty() = default;
    ~WebasmPty() override;

    Result<void> init(Config* config);

    size_t read(char* buf, size_t maxLen) override;
    void write(const char* data, size_t len) override;
    void resize(uint32_t cols, uint32_t rows) override;
    void stop() override;
    PtyPollSource* pollSource() override;

private:
    uint32_t _cols = 80;
    uint32_t _rows = 24;
    bool _running = false;
    WebasmPtyPollSource _pollSource;
};

} // namespace yetty
