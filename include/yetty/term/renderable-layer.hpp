#pragma once

#include <yetty/term/terminal-screen-render-context.hpp>
#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>

namespace yetty {

class TerminalScreen;
class GpuResourceBinder;

// Base class for terminal rendering layers.
// Each layer renders on top of the previous layer's output.
// Each layer owns a GpuResourceBinder and provides its own GpuResourceSet internally.
class RenderableLayer : public core::FactoryObject<RenderableLayer> {
public:
    virtual ~RenderableLayer() = default;

    virtual Result<void> render(const TerminalScreenRenderContext& terminalScreenRenderContext) = 0;
    virtual bool isDirty() const = 0;

protected:
    RenderableLayer() = default;

    RenderableLayer* _previousLayer = nullptr;
    TerminalScreen* _terminalScreen = nullptr;
    GpuResourceBinder* _binder = nullptr;
};

} // namespace yetty
