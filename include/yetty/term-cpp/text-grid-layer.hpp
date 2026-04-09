#pragma once

#include <yetty/term/renderable-layer.hpp>
#include <yetty/core/result.hpp>

namespace yetty {

class TerminalScreen;
class Font;
class GpuAllocator;
class ShaderManager;

// Layer 0: Renders the terminal text grid (VTermScreenCell buffer).
class TextGridLayer : public RenderableLayer {
public:
    ~TextGridLayer() override;

    static Result<TextGridLayer*> createImpl(TerminalScreen* terminalScreen,
                                              RenderableLayer* previousLayer,
                                              const TerminalScreenRenderContext& terminalScreenRenderContext);

protected:
    TextGridLayer() = default;
};

} // namespace yetty
