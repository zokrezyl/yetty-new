#include <yetty/term/terminal.hpp>
#include <yetty/term/terminal-screen.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/platform/pty.hpp>
#include <ytrace/ytrace.hpp>

namespace yetty {

class TerminalImpl : public Terminal {
public:
  explicit TerminalImpl(const YettyContext &yettyCtx) : _yettyCtx(yettyCtx) {}

  ~TerminalImpl() override {
    _pty->stop();
    delete _pty;
    delete _screen;
  }

  const char *typeName() const override { return "Terminal"; }

  Result<void> init() {
    ydebug("Terminal::init starting");

    auto ptyResult = _yettyCtx.appCtx->ptyFactory->createPty();
    if (!ptyResult)
      return Err<void>("Failed to create PTY", ptyResult);
    _pty = *ptyResult;
    ydebug("Terminal: PTY created");

    auto screenResult = TerminalScreen::create(80, 24, _pty);
    if (!screenResult)
      return Err<void>("Failed to create TerminalScreen", screenResult);
    _screen = *screenResult;
    ydebug("Terminal: TerminalScreen created");

    auto *eventLoop = _yettyCtx.appCtx->eventLoop;

    // Setup PTY poll - TerminalScreen receives PTY data
    auto ptyPollResult = eventLoop->createPtyPoll(_pty->pollSource());
    if (!ptyPollResult)
      return Err<void>("Failed to create PTY poll", ptyPollResult);
    auto ptyPollId = *ptyPollResult;
    ydebug("Terminal: PTY poll created");

    if (auto res = eventLoop->registerPollListener(ptyPollId, _screen); !res)
      return Err<void>("Failed to register PTY poll listener", res);

    if (auto res = eventLoop->startPoll(ptyPollId); !res)
      return Err<void>("Failed to start PTY poll", res);
    ydebug("Terminal: PTY poll started");

    // Setup PlatformInputPipe poll - TerminalScreen receives platform events
    auto *pipe = _yettyCtx.appCtx->platformInputPipe;
    auto pipePollResult = eventLoop->createPlatformInputPipePoll(pipe);
    if (!pipePollResult)
      return Err<void>("Failed to create PlatformInputPipe poll", pipePollResult);
    auto pipePollId = *pipePollResult;
    ydebug("Terminal: PlatformInputPipe poll created");

    if (auto res = eventLoop->registerPlatformInputPipePollListener(pipePollId, _screen); !res)
      return Err<void>("Failed to register PlatformInputPipe poll listener", res);

    if (auto res = eventLoop->startPlatformInputPipePoll(pipePollId); !res)
      return Err<void>("Failed to start PlatformInputPipe poll", res);
    ydebug("Terminal: PlatformInputPipe poll started");

    ydebug("Terminal::init complete");
    return Ok();
  }

  Result<void> run() override {
    ydebug("Terminal::run - starting EventLoop");
    _yettyCtx.appCtx->eventLoop->start();
    ydebug("Terminal::run - EventLoop stopped");
    return Ok();
  }

private:
  YettyContext _yettyCtx;
  TerminalScreen *_screen = nullptr;
  Pty *_pty = nullptr;
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
