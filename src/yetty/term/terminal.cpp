#include <yetty/term/terminal.hpp>
#include <yetty/term/terminal-screen.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/platform/pty.hpp>

namespace yetty {

class TerminalImpl : public Terminal {
public:
  explicit TerminalImpl(const YettyContext &yettyCtx) : _yettyCtx(yettyCtx) {}

  ~TerminalImpl() override {
    _yettyCtx.appCtx->eventLoop->destroyPoll(_ptyPollId);
    _pty->stop();
    delete _pty;
    delete _screen;
  }

  const char *typeName() const override { return "Terminal"; }

  Result<void> init() {
    auto ptyResult = _yettyCtx.appCtx->ptyFactory->createPty();
    if (!ptyResult)
      return Err<void>("Failed to create PTY", ptyResult);
    _pty = *ptyResult;

    auto screenResult = TerminalScreen::create(80, 24, _pty);
    if (!screenResult)
      return Err<void>("Failed to create TerminalScreen", screenResult);
    _screen = *screenResult;

    _screen->setOutputCallback([this](const char *data, size_t len) {
      _pty->write(data, len);
    });

    auto *eventLoop = _yettyCtx.appCtx->eventLoop;

    auto pollResult = eventLoop->createPtyPoll(_pty->pollSource());
    if (!pollResult)
      return Err<void>("Failed to create PTY poll", pollResult);
    _ptyPollId = *pollResult;

    if (auto res = eventLoop->registerPollListener(
            _ptyPollId, static_cast<core::EventListener *>(_screen));
        !res)
      return Err<void>("Failed to register PTY poll listener", res);

    if (auto res = eventLoop->startPoll(_ptyPollId); !res)
      return Err<void>("Failed to start PTY poll", res);

    return Ok();
  }

  Result<void> run() override {
    _yettyCtx.appCtx->eventLoop->start();
    return Ok();
  }

private:
  YettyContext _yettyCtx;
  TerminalScreen *_screen = nullptr;
  Pty *_pty = nullptr;
  core::PollId _ptyPollId = -1;
};

Result<Terminal *> Terminal::createImpl(const YettyContext &yettyCtx) {
  auto *term = new TerminalImpl(yettyCtx);
  if (auto res = term->init(); !res) {
    delete term;
    return Err<Terminal *>("Terminal init failed", res);
  }
  return Ok(static_cast<Terminal *>(term));
}

} // namespace yetty
