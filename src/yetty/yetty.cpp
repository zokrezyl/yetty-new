#include <yetty/yetty.hpp>
#include <yetty/yetty-context.hpp>
#include <yetty/term/terminal.hpp>

namespace yetty {

class YettyImpl : public Yetty {
public:
  explicit YettyImpl(const AppContext &appCtx) : _appCtx(appCtx) {
    _yettyCtx.appCtx = &_appCtx;
  }

  ~YettyImpl() override { delete _terminal; }

  const char *typeName() const override { return "Yetty"; }

  Result<void> init() {
    auto termResult = Terminal::create(_yettyCtx);
    if (!termResult) {
      return Err<void>("Failed to create Terminal", termResult);
    }
    _terminal = *termResult;
    return Ok();
  }

  Result<void> run() override {
    if (_terminal) {
      return _terminal->run();
    }
    return Ok();
  }

private:
  AppContext _appCtx;
  YettyContext _yettyCtx;
  Terminal *_terminal = nullptr;
};

Result<Yetty *> Yetty::createImpl(const AppContext &appCtx) {
  auto *yetty = new YettyImpl(appCtx);
  if (auto res = yetty->init(); !res) {
    delete yetty;
    return Err<Yetty *>("Yetty init failed", res);
  }
  return Ok(static_cast<Yetty *>(yetty));
}

} // namespace yetty
