#include <yetty/term/terminal.hpp>
#include <yetty/term/terminal-screen.hpp>
#include <yetty/term/terminal-screen-context.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/platform/pty.hpp>
#include <ytrace/ytrace.hpp>

namespace yetty {

class TerminalImpl : public Terminal {
public:
  explicit TerminalImpl(const YettyContext& yettyContext)
      : _yettyContext(yettyContext) {}

  ~TerminalImpl() override {
    _pty->stop();
    delete _pty;
    delete _screen;
    delete _eventLoop;
  }

  const char* typeName() const override { return "Terminal"; }

  Result<void> init() {
    ydebug("Terminal::init starting");

    // Create EventLoop
    auto eventLoopResult = core::EventLoop::create();
    if (!eventLoopResult)
      return Err<void>("Failed to create EventLoop", eventLoopResult);
    _eventLoop = *eventLoopResult;
    ydebug("Terminal: EventLoop created");

    // Create PTY
    auto ptyResult = _yettyContext.appContext.ptyFactory->createPty();
    if (!ptyResult)
      return Err<void>("Failed to create PTY", ptyResult);
    _pty = *ptyResult;
    ydebug("Terminal: PTY created");

    // Build TerminalScreenContext
    TerminalScreenContext screenContext;
    screenContext.yettyContext = _yettyContext;  // COPY
    screenContext.pty = _pty;
    // Note: screenContext.gpuContext is populated by TerminalScreen

    auto screenResult = TerminalScreen::create(80, 24, screenContext);
    if (!screenResult)
      return Err<void>("Failed to create TerminalScreen", screenResult);
    _screen = *screenResult;
    ydebug("Terminal: TerminalScreen created");

    // Setup PTY poll - TerminalScreen receives PTY data
    auto ptyPollResult = _eventLoop->createPtyPoll(_pty->pollSource());
    if (!ptyPollResult)
      return Err<void>("Failed to create PTY poll", ptyPollResult);
    auto ptyPollId = *ptyPollResult;
    ydebug("Terminal: PTY poll created");

    if (auto res = _eventLoop->registerPollListener(ptyPollId, _screen); !res)
      return Err<void>("Failed to register PTY poll listener", res);

    if (auto res = _eventLoop->startPoll(ptyPollId); !res)
      return Err<void>("Failed to start PTY poll", res);
    ydebug("Terminal: PTY poll started");

    // Setup PlatformInputPipe poll - TerminalScreen receives platform events
    auto* pipe = _yettyContext.appContext.platformInputPipe;
    auto pipePollResult = _eventLoop->createPlatformInputPipePoll(pipe);
    if (!pipePollResult)
      return Err<void>("Failed to create PlatformInputPipe poll", pipePollResult);
    auto pipePollId = *pipePollResult;
    ydebug("Terminal: PlatformInputPipe poll created");

    if (auto res = _eventLoop->registerPlatformInputPipePollListener(pipePollId, _screen); !res)
      return Err<void>("Failed to register PlatformInputPipe poll listener", res);

    if (auto res = _eventLoop->startPlatformInputPipePoll(pipePollId); !res)
      return Err<void>("Failed to start PlatformInputPipe poll", res);
    ydebug("Terminal: PlatformInputPipe poll started");

    ydebug("Terminal::init complete");
    return Ok();
  }

  Result<void> run() override {
    ydebug("Terminal::run - starting EventLoop");
    _eventLoop->start();
    ydebug("Terminal::run - EventLoop stopped");
    return Ok();
  }

private:
  YettyContext _yettyContext;  // COPY of Yetty's context
  core::EventLoop* _eventLoop = nullptr;
  TerminalScreen* _screen = nullptr;
  Pty* _pty = nullptr;
};

Result<Terminal*> Terminal::createImpl(const YettyContext& yettyCtx) {
  auto* term = new TerminalImpl(yettyCtx);
  if (auto res = term->init(); !res) {
    delete term;
    return Err<Terminal*>("Terminal init failed", res);
  }
  return Ok(static_cast<Terminal*>(term));
}

} // namespace yetty
