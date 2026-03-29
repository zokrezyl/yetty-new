#pragma once

#include <yetty/app-context.hpp>
#include <yetty/yetty-gpu-context.hpp>

namespace yetty {

class SharedBindGroup;
class MsMsdfFont;

// Yetty-level context — created by Yetty, passed to children (Terminal, etc).
// Contains copy of AppContext plus Yetty's GPU state and shared resources.
struct YettyContext {
    AppContext appContext;              // COPY of platform's context
    YettyGpuContext gpuContext;         // Yetty's GPU state (adapter, device, queue)
    SharedBindGroup* sharedBindGroup = nullptr;  // owned by Yetty
    MsMsdfFont* defaultMsMsdfFont = nullptr;     // owned by Yetty (if created)
};

} // namespace yetty
