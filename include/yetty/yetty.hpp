#pragma once

#include <yetty/app-context.hpp>
#include <yetty/core/factory-object.hpp>

namespace yetty {

class Yetty : public core::FactoryObject<Yetty> {
public:
  static Result<Yetty *> createImpl(const AppContext &appCtx);

  virtual ~Yetty() = default;

  virtual Result<void> run() = 0;

protected:
  Yetty() = default;
};

} // namespace yetty
