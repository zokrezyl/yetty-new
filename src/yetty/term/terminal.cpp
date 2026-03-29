#include <yetty/term/terminal.hpp>
#include <yetty/term/terminal-screen.hpp>
#include <yetty/term/terminal-context.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/platform/pty.hpp>
#include <ytrace/ytrace.hpp>

namespace yetty {

class TerminalImpl : public Terminal, public core::EventListener {
public:
  explicit TerminalImpl(const YettyContext& yettyContext) {
    // Build our context from parent - stores ONLY our level
    _terminalContext.yettyContext = yettyContext;
  }

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
    auto ptyResult = _terminalContext.yettyContext.appContext.ptyFactory->createPty();
    if (!ptyResult)
      return Err<void>("Failed to create PTY", ptyResult);
    _pty = *ptyResult;
    ydebug("Terminal: PTY created");

    // Complete our context with owned objects
    _terminalContext.eventLoop = _eventLoop;
    _terminalContext.pty = _pty;

    auto screenResult = TerminalScreen::create(80, 24, _terminalContext);
    if (!screenResult)
      return Err<void>("Failed to create TerminalScreen", screenResult);
    _screen = *screenResult;
    ydebug("Terminal: TerminalScreen created");

    // Setup PTY poll - Terminal handles PTY data, forwards to TerminalScreen
    auto ptyPollResult = _eventLoop->createPtyPoll(_pty->pollSource());
    if (!ptyPollResult)
      return Err<void>("Failed to create PTY poll", ptyPollResult);
    _ptyPollId = *ptyPollResult;
    ydebug("Terminal: PTY poll created");

    if (auto res = _eventLoop->registerPollListener(_ptyPollId, this); !res)
      return Err<void>("Failed to register PTY poll listener", res);

    if (auto res = _eventLoop->startPoll(_ptyPollId); !res)
      return Err<void>("Failed to start PTY poll", res);
    ydebug("Terminal: PTY poll started");

    // Setup PlatformInputPipe poll - TerminalScreen receives platform events
    auto* pipe = _terminalContext.yettyContext.appContext.platformInputPipe;
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

    // Register TerminalScreen for Render events
    if (auto res = _eventLoop->registerListener(core::Event::Type::Render, _screen); !res)
      return Err<void>("Failed to register Render listener", res);
    ydebug("Terminal: Render listener registered");

    ydebug("Terminal::init complete");
    return Ok();
  }

  Result<void> run() override {
    ydebug("Terminal::run - starting EventLoop");
    _eventLoop->start();
    ydebug("Terminal::run - EventLoop stopped");
    return Ok();
  }

  // EventListener - handle PTY data
  Result<bool> onEvent(const core::Event& event) override {
    if (event.type == core::Event::Type::PollReadable) {
      char buf[65536];
      while (true) {
        size_t n = _pty->read(buf, sizeof(buf));
        if (n == 0)
          break;
        _screen->write(buf, n);
      }
      _eventLoop->requestRender();
      return Ok(true);
    }
    return Err<bool>("Terminal: unexpected event type " + std::to_string(static_cast<int>(event.type)));
  }

private:
  // Context - stores ONLY our level, access parent via _terminalContext.yettyContext
  TerminalContext _terminalContext;
  core::EventLoop* _eventLoop = nullptr;
  TerminalScreen* _screen = nullptr;
  Pty* _pty = nullptr;
  core::PollId _ptyPollId = -1;
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
