// WebAssembly pty-io.cpp - Local PTY via JSLinux iframe postMessage
//
// Architecture:
// - WebasmPty creates JSLinux iframe and communicates via postMessage
// - JS listener setup passes 'this' pointer to JavaScript
// - JS calls webpty_on_data(ptyPointer, data, len) with the pointer
// - No globals - Pty pointer flows through: C++ → JS → C callback

#include <yetty/platform/pty.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/config.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace yetty {

// Forward declaration
class WebasmPty;

// WebasmPtyPollSource - holds receive buffer
class WebasmPtyPollSource : public PtyPollSource {
public:
    void write(const char* data, size_t len) {
        if (len == 0) return;
        std::lock_guard<std::mutex> lock(_mutex);
        _buffer.insert(_buffer.end(), data, data + len);
    }

    size_t read(char* buf, size_t maxLen) {
        std::lock_guard<std::mutex> lock(_mutex);
        size_t toRead = std::min(maxLen, _buffer.size());
        if (toRead == 0) return 0;
        std::memcpy(buf, _buffer.data(), toRead);
        _buffer.erase(_buffer.begin(), _buffer.begin() + toRead);
        return toRead;
    }

    bool hasData() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return !_buffer.empty();
    }

private:
    mutable std::mutex _mutex;
    std::vector<char> _buffer;
};

class WebasmPty : public Pty {
public:
    WebasmPty() = default;

    ~WebasmPty() override {
        stop();
    }

    Result<void> init(Config* config) {
        _cols = static_cast<uint32_t>(config->get<int>("terminal/cols", 80));
        _rows = static_cast<uint32_t>(config->get<int>("terminal/rows", 24));
        _running = true;

        ydebug("WebasmPty: Starting ({}x{})", _cols, _rows);

        // Create hidden iframe for JSLinux VM and set up message listener
        // Pass 'this' pointer to JS so callback can find us without globals
        EM_ASM({
            var ptyPointer = $0;
            var cols = $1;
            var rows = $2;

            // Create iframe
            var iframe = document.createElement('iframe');
            iframe.id = 'jslinux-pty';
            iframe.style.cssText = 'display:none;';
            iframe.src = 'jslinux/vm-bridge.html?' +
                         'cols=' + cols + '&rows=' + rows +
                         '&cpu=x86_64&mem=256';
            document.body.appendChild(iframe);

            // Set up message listener with ptyPointer captured
            window.addEventListener('message', function(e) {
                if (e.data && e.data.type === 'term-output') {
                    var data = e.data.data;
                    if (!data || data.length === 0) return;
                    var encoder = new TextEncoder();
                    var bytes = encoder.encode(data);
                    if (bytes.length === 0) return;
                    var dataPointer = Module._malloc(bytes.length);
                    if (dataPointer === 0) return;
                    Module.HEAPU8.set(bytes, dataPointer);
                    Module._webpty_on_data(ptyPointer, dataPointer, bytes.length);
                    Module._free(dataPointer);
                }
            });
        }, this, _cols, _rows);

        return Ok();
    }

    size_t read(char* buf, size_t maxLen) override {
        return _pollSource.read(buf, maxLen);
    }

    void write(const char* data, size_t len) override {
        if (!_running || len == 0) return;

        EM_ASM({
            var data = UTF8ToString($0, $1);
            var iframe = document.getElementById('jslinux-pty');
            if (iframe && iframe.contentWindow) {
                iframe.contentWindow.postMessage({
                    type: 'term-input',
                    data: data
                }, '*');
            }
        }, data, len);
    }

    void resize(uint32_t cols, uint32_t rows) override {
        _cols = cols;
        _rows = rows;

        EM_ASM({
            var cols = $0;
            var rows = $1;
            var iframe = document.getElementById('jslinux-pty');
            if (iframe && iframe.contentWindow) {
                iframe.contentWindow.postMessage({
                    type: 'term-resize',
                    cols: cols,
                    rows: rows
                }, '*');
            }
        }, cols, rows);
    }

    bool isRunning() const override {
        return _running;
    }

    void stop() override {
        if (!_running) return;
        _running = false;

        ydebug("WebasmPty: Stopping");

        EM_ASM({
            var iframe = document.getElementById('jslinux-pty');
            if (iframe) {
                iframe.remove();
            }
        });
    }

    PtyPollSource* pollSource() override {
        return &_pollSource;
    }

    // Called from C export when JS receives term-output
    void onData(const char* data, size_t len) {
        _pollSource.write(data, len);
    }

private:
    uint32_t _cols = 80;
    uint32_t _rows = 24;
    bool _running = false;
    WebasmPtyPollSource _pollSource;
};

class WebasmPtyFactory : public PtyFactory {
public:
    explicit WebasmPtyFactory(Config* config) : _config(config) {}

    Result<Pty*> createPty() override {
        auto* pty = new WebasmPty();
        if (auto res = pty->init(_config); !res) {
            delete pty;
            return Err<Pty*>("Failed to create WebasmPty", res);
        }
        return Ok(static_cast<Pty*>(pty));
    }

private:
    Config* _config;
};

Result<PtyFactory*> PtyFactory::createImpl(Config* config, void*) {
    return Ok(static_cast<PtyFactory*>(new WebasmPtyFactory(config)));
}

} // namespace yetty

// =============================================================================
// C export - receives Pty pointer from JS, no globals needed
// =============================================================================

extern "C" {

EMSCRIPTEN_KEEPALIVE
void webpty_on_data(yetty::WebasmPty* pty, const char* data, int len) {
    if (pty && data && len > 0) {
        pty->onData(data, static_cast<size_t>(len));
    }
}

} // extern "C"
