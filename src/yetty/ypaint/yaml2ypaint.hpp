#pragma once

#include "ypaint-buffer.hpp"
#include "animation.hpp"
#include <string>

namespace yetty::ypaint {

// Returns the animation (null if YAML has no animation blocks)
Result<animation::Animation::Ptr> parseYPaintYAML(YPaintBuffer::Ptr buffer,
                                                  const std::string& yaml);

} // namespace yetty::ypaint
