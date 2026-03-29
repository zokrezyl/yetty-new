#pragma once

#include <yetty/term/renderable-layer.hpp>
#include <yetty/core/result.hpp>

namespace yetty {

class TerminalScreen;
class Font;
class GpuAllocator;
class ShaderManager;

// Layer 0: Renders the terminal text grid (TextCell buffer).
class TextGridLayer : public RenderableLayer {
public:
    static Result<TextGridLayer*> createImpl(TerminalScreen* terminalScreen,
                                              RenderableLayer* previousLayer,
                                              const TerminalScreenRenderContext& ctx);

    void render(const TerminalScreenRenderContext& ctx) override;
    bool isDirty() const override;

protected:
    TextGridLayer() = default;
};

} // namespace yetty
