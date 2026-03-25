#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/yetty-context.hpp>

namespace yetty {

// Terminal - owns PTY + TerminalScreen
// Handles shell communication and terminal emulation
class Terminal : public core::FactoryObject<Terminal> {
public:
  static Result<Terminal *> createImpl(const YettyContext &ctx);

  virtual ~Terminal() = default;

  virtual Result<void> run() = 0;

protected:
  Terminal() = default;
};

} // namespace yetty
