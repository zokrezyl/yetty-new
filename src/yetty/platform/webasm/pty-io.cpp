// WebAssembly pty-io.cpp - PTY I/O via JSLinux iframe postMessage

#include <yetty/platform/pty.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace yetty {

// Global registry of PTY instances for JS interop callbacks
static std::unordered_map<uint32_t, class WebasmPty*> g_ptyInstances;
static uint32_t g_nextPtyId = 1;

class WebasmPty : public Pty {
public:
    WebasmPty() : _ptyId(g_nextPtyId++) {
        g_ptyInstances[_ptyId] = this;
    }

    ~WebasmPty() override {
        stop();
        g_ptyInstances.erase(_ptyId);
    }

    Result<void> init(const PtyConfig& config) {
        _vmConfig = config.shell;
        _cols = config.cols;
        _rows = config.rows;
        _running = true;

        ydebug("WebasmPty[{}]: Starting with config '{}' ({}x{})",
               _ptyId, _vmConfig, _cols, _rows);

        // Create hidden iframe for JSLinux VM
        EM_ASM({
            var ptyId = $0;
            var vmConfig = UTF8ToString($1);
            var cols = $2;
            var rows = $3;

            var iframe = document.createElement('iframe');
            iframe.id = 'jslinux-pty-' + ptyId;
            iframe.style.cssText = 'display:none;';
            iframe.src = 'jslinux/vm-bridge.html?ptyId=' + ptyId +
                         '&url=' + encodeURIComponent(vmConfig) +
                         '&cols=' + cols + '&rows=' + rows +
                         '&cpu=x86_64&mem=256';
            document.body.appendChild(iframe);
        }, _ptyId, _vmConfig.c_str(), _cols, _rows);

        return Ok();
    }

    size_t read(char* buf, size_t maxLen) override {
        std::lock_guard<std::mutex> lock(_bufferMutex);

        size_t toRead = std::min(maxLen, _buffer.size());
        if (toRead == 0) return 0;

        std::memcpy(buf, _buffer.data(), toRead);
        _buffer.erase(_buffer.begin(), _buffer.begin() + toRead);

        return toRead;
    }

    void write(const char* data, size_t len) override {
        if (!_running || len == 0) return;

        EM_ASM({
            var ptyId = $0;
            var data = UTF8ToString($1, $2);
            var iframe = document.getElementById('jslinux-pty-' + ptyId);
            if (iframe && iframe.contentWindow) {
                iframe.contentWindow.postMessage({
                    type: 'term-input',
                    ptyId: ptyId,
                    data: data
                }, '*');
            }
        }, _ptyId, data, len);
    }

    void resize(uint32_t cols, uint32_t rows) override {
        _cols = cols;
        _rows = rows;

        EM_ASM({
            var ptyId = $0;
            var cols = $1;
            var rows = $2;
            var iframe = document.getElementById('jslinux-pty-' + ptyId);
            if (iframe && iframe.contentWindow) {
                iframe.contentWindow.postMessage({
                    type: 'term-resize',
                    ptyId: ptyId,
                    cols: cols,
                    rows: rows
                }, '*');
            }
        }, _ptyId, cols, rows);
    }

    bool isRunning() const override {
        return _running;
    }

    void stop() override {
        if (!_running) return;
        _running = false;

        ydebug("WebasmPty[{}]: Stopping", _ptyId);

        EM_ASM({
            var ptyId = $0;
            var iframe = document.getElementById('jslinux-pty-' + ptyId);
            if (iframe) {
                iframe.remove();
            }
        }, _ptyId);

        if (_exitCallback) {
            _exitCallback(0);
        }
    }

    void setDataAvailableCallback(DataAvailableCallback cb) override {
        _dataAvailableCallback = std::move(cb);
    }

    void setExitCallback(ExitCallback cb) override {
        _exitCallback = std::move(cb);
    }

    // Called from JS interop when VM produces output
    void pushData(const char* data, size_t len) {
        if (len == 0) return;

        {
            std::lock_guard<std::mutex> lock(_bufferMutex);
            _buffer.insert(_buffer.end(), data, data + len);
        }

        if (_dataAvailableCallback) {
            _dataAvailableCallback();
        }
    }

private:
    uint32_t _ptyId;
    std::string _vmConfig;
    uint32_t _cols = 80;
    uint32_t _rows = 24;
    bool _running = false;

    std::mutex _bufferMutex;
    std::vector<char> _buffer;

    DataAvailableCallback _dataAvailableCallback;
    ExitCallback _exitCallback;
};

class WebasmPtyFactory : public PtyFactory {
public:
    Result<Pty::Ptr> create(const PtyConfig& config, void* /*osSpecific*/) override {
        auto pty = std::make_shared<WebasmPty>();
        if (auto res = pty->init(config); !res) {
            return Err<Pty::Ptr>("Failed to create WebasmPty", res);
        }
        return Ok<Pty::Ptr>(pty);
    }
};

PtyFactory::Ptr createPtyFactory() {
    return std::make_shared<WebasmPtyFactory>();
}

} // namespace yetty

// =============================================================================
// Exported C functions for JavaScript interop
// =============================================================================

extern "C" {

EMSCRIPTEN_KEEPALIVE
void webpty_on_data(uint32_t ptyId, const char* data, int len) {
    auto it = yetty::g_ptyInstances.find(ptyId);
    if (it != yetty::g_ptyInstances.end()) {
        it->second->pushData(data, static_cast<size_t>(len));
    }
}

} // extern "C"

// Initialize postMessage listener at startup
static struct WebPTYInit {
    WebPTYInit() {
        EM_ASM({
            window.addEventListener('message', function(e) {
                if (e.data && e.data.type === 'term-output' && e.data.ptyId) {
                    var data = e.data.data;
                    if (!data || data.length === 0) return;
                    var ptyId = parseInt(e.data.ptyId, 10);
                    if (isNaN(ptyId)) return;
                    var encoder = new TextEncoder();
                    var bytes = encoder.encode(data);
                    if (bytes.length === 0) return;
                    var ptr = Module._malloc(bytes.length);
                    if (ptr === 0) return;
                    Module.HEAPU8.set(bytes, ptr);
                    Module._webpty_on_data(ptyId, ptr, bytes.length);
                    Module._free(ptr);
                }
            });
        });
    }
} g_webPtyInit;
