// WebAssembly PTY factory + C export

#include "webasm-pty.hpp"
#include "webasm-pty-poll-source.hpp"
#include <yetty/platform/pty-factory.hpp>
#include <yetty/config.hpp>
#include <emscripten/emscripten.h>

namespace yetty {

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
// C export - called by JS when data arrives in buffer
// =============================================================================

extern "C" {

EMSCRIPTEN_KEEPALIVE
void webpty_poll_source_notify(yetty::WebasmPtyPollSource* pollSource) {
    if (pollSource) {
        pollSource->notify();
    }
}

} // extern "C"
