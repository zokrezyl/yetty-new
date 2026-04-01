// WebAssembly WebasmPty implementation
//
// See docs/platform-pty.md for architecture details.

#include "webasm-pty.hpp"
#include <yetty/config.hpp>
#include <ytrace/ytrace.hpp>
#include <emscripten/emscripten.h>

namespace yetty {

WebasmPty::~WebasmPty() {
    stop();
}

Result<void> WebasmPty::init(Config* config) {
    _cols = static_cast<uint32_t>(config->get<int>("terminal/cols", 80));
    _rows = static_cast<uint32_t>(config->get<int>("terminal/rows", 24));
    _running = true;

    ydebug("WebasmPty: Starting ({}x{})", _cols, _rows);

    // Set up JS buffer and message listener in parent window,
    // then create iframe for JSLinux VM
    EM_ASM({
        var pollSourcePointer = $0;
        var cols = $1;
        var rows = $2;

        // Buffer in parent window (like kernel buffer on Unix)
        window.ptyBuffer = "";

        // Read from buffer - called by C++ via EM_ASM
        window.pty_read_buffer = function(maxLen) {
            var chunk = window.ptyBuffer.substring(0, maxLen);
            window.ptyBuffer = window.ptyBuffer.substring(chunk.length);
            return chunk;
        };

        // Message listener - receives data from iframe, buffers it, notifies PollSource
        window.addEventListener('message', function(e) {
            if (e.data && e.data.type === 'term-output') {
                var data = e.data.data;
                if (!data || data.length === 0) return;
                window.ptyBuffer += data;
                Module._webpty_poll_source_notify(pollSourcePointer);
            }
        });

        // Create iframe for JSLinux VM
        var iframe = document.createElement('iframe');
        iframe.id = 'jslinux-pty';
        iframe.style.cssText = 'display:none;';
        iframe.src = 'jslinux/vm-bridge.html?' +
                     'cols=' + cols + '&rows=' + rows +
                     '&cpu=x86_64&mem=256';
        document.body.appendChild(iframe);
    }, &_pollSource, _cols, _rows);

    return Ok();
}

size_t WebasmPty::read(char* buf, size_t maxLen) {
    if (!_running || maxLen == 0) return 0;

    // Read from JS buffer via EM_ASM
    int bytesRead = EM_ASM_INT({
        var maxLen = $1;
        var chunk = window.pty_read_buffer(maxLen);
        if (chunk.length === 0) return 0;

        // Encode to UTF-8 and copy to C++ buffer
        var encoder = new TextEncoder();
        var bytes = encoder.encode(chunk);
        var len = Math.min(bytes.length, maxLen);
        HEAPU8.set(bytes.subarray(0, len), $0);
        return len;
    }, buf, static_cast<int>(maxLen));

    return static_cast<size_t>(bytesRead > 0 ? bytesRead : 0);
}

void WebasmPty::write(const char* data, size_t len) {
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

void WebasmPty::resize(uint32_t cols, uint32_t rows) {
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

void WebasmPty::stop() {
    if (!_running) return;
    _running = false;

    ydebug("WebasmPty: Stopping");

    EM_ASM({
        var iframe = document.getElementById('jslinux-pty');
        if (iframe) {
            iframe.remove();
        }
        window.ptyBuffer = "";
    });
}

PtyPollSource* WebasmPty::pollSource() {
    return &_pollSource;
}

} // namespace yetty
