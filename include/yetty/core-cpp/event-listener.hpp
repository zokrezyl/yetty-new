#pragma once

#include <yetty/core/event.hpp>
#include <yetty/core/result.hpp>

namespace yetty {
namespace core {

class EventListener {
public:
  virtual ~EventListener() = default;

  // Handle event. Returns Ok(true) if consumed, Ok(false) if not, Err on
  // failure.
  virtual Result<bool> onEvent(const Event &event) = 0;
};

} // namespace core
} // namespace yetty
