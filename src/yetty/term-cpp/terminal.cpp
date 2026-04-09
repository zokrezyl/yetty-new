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

    // Create EventLoop with pipe
    auto* pipe = _terminalContext.yettyContext.appContext.platformInputPipe;
    auto eventLoopResult = core::EventLoop::create(pipe);
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

    // Register TerminalScreen for platform input events (dispatched by type)
    if (auto res = _eventLoop->registerListener(core::Event::Type::KeyDown, _screen); !res)
      return Err<void>("Failed to register KeyDown listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::KeyUp, _screen); !res)
      return Err<void>("Failed to register KeyUp listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::Char, _screen); !res)
      return Err<void>("Failed to register Char listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::MouseDown, _screen); !res)
      return Err<void>("Failed to register MouseDown listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::MouseUp, _screen); !res)
      return Err<void>("Failed to register MouseUp listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::MouseMove, _screen); !res)
      return Err<void>("Failed to register MouseMove listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::Scroll, _screen); !res)
      return Err<void>("Failed to register Scroll listener", res);
    if (auto res = _eventLoop->registerListener(core::Event::Type::Resize, _screen); !res)
      return Err<void>("Failed to register Resize listener", res);
    ydebug("Terminal: input event listeners registered");

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

  // EventListener - currently unused, but kept for future use
  Result<bool> onEvent(const core::Event& event) override {
    return Err<bool>("Terminal: unexpected event type " + std::to_string(static_cast<int>(event.type)));
  }

private:
  // Context - stores ONLY our level, access parent via _terminalContext.yettyContext
  TerminalContext _terminalContext;
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
