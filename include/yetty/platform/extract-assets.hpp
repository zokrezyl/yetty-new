#pragma once

#include <yetty/core/result.hpp>

namespace yetty {

class Config;

namespace platform {

// Extract embedded assets (shaders, fonts) to the cache directory.
// Cache dir is derived from Config paths. Skips if already up-to-date.
Result<void> extractAssets(Config* config);

} // namespace platform
} // namespace yetty
