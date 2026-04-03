#include <yetty/term/text-grid-layer.hpp>
#include <yetty/term/terminal-screen.hpp>
#include <yetty/config.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/gpu-resource-binder.hpp>
#include <yetty/shader-manager.hpp>
#include <yetty/font/font.hpp>
#include <ytrace/ytrace.hpp>

namespace yetty {

// Define destructor out-of-line for typeinfo
TextGridLayer::~TextGridLayer() = default;

//=============================================================================
// TextGridLayerImpl
//=============================================================================

class TextGridLayerImpl : public TextGridLayer {
public:
    TextGridLayerImpl() = default;
    ~TextGridLayerImpl() override { cleanup(); }

    Result<void> init(TerminalScreen* terminalScreen,
                      RenderableLayer* previousLayer,
                      const TerminalScreenRenderContext& terminalScreenRenderContext) {
        _terminalScreen = terminalScreen;
        _previousLayer = previousLayer;
        _terminalScreenRenderContext = terminalScreenRenderContext;

        auto& yettyContext = terminalScreenRenderContext.terminalScreenContext.terminalContext.yettyContext;
        auto& yettyGpuContext = yettyContext.yettyGpuContext;

        // Create GpuAllocator
        auto gpuAllocatorResult = GpuAllocator::create(yettyGpuContext.device);
        if (!gpuAllocatorResult) {
            return Err<void>("Failed to create GpuAllocator", gpuAllocatorResult);
        }
        _gpuAllocator = *gpuAllocatorResult;

        // Create ShaderManager
        std::string shadersDir = yettyContext.appContext.config->get<std::string>("paths/shaders", "");
        auto shaderManagerResult = ShaderManager::create(shadersDir);
        if (!shaderManagerResult) {
            return Err<void>("Failed to create ShaderManager", shaderManagerResult);
        }
        _shaderManager = *shaderManagerResult;

        // Get font from TerminalScreen (owned by TerminalScreen)
        _font = _terminalScreen->getFont();
        if (!_font) {
            return Err<void>("TerminalScreen has no font");
        }

        // Create GpuResourceBinder
        auto gpuResourceBinderResult = GpuResourceBinder::create(yettyGpuContext, _gpuAllocator, _shaderManager);
        if (!gpuResourceBinderResult) {
            return Err<void>("Failed to create GpuResourceBinder", gpuResourceBinderResult);
        }
        _gpuResourceBinder = *gpuResourceBinderResult;

        ydebug("TextGridLayer initialized");
        return Ok();
    }

    //=========================================================================
    // RenderableLayer interface
    //=========================================================================

    Result<void> render(const TerminalScreenRenderContext& terminalScreenRenderContext) override {
        // Update uniforms from terminal state
        updateUniforms(terminalScreenRenderContext);

        // Submit resource sets every frame (creates on first call, uploads data on subsequent)
        if (auto result = _gpuResourceBinder->submitGpuResourceSet(getGridUniformsResourceSet()); !result) {
            return Err<void>("Failed to submit grid uniforms", result);
        }
        if (auto result = _gpuResourceBinder->submitGpuResourceSet(_font->getGpuResourceSet()); !result) {
            return Err<void>("Failed to submit font resources", result);
        }
        if (auto result = _gpuResourceBinder->submitGpuResourceSet(getCellBufferResourceSet()); !result) {
            return Err<void>("Failed to submit cell buffer", result);
        }

        // Finalize on first render (generates WGSL bindings, compiles shaders, creates pipeline)
        if (!_finalized) {
            if (auto result = _gpuResourceBinder->finalize(); !result) {
                return Err<void>("Failed to finalize binder", result);
            }
            _finalized = true;
        }

        WGPURenderPipeline pipeline = _gpuResourceBinder->getPipeline();
        if (!pipeline) {
            return Err<void>("Pipeline not ready");
        }

        // Set pipeline and bind resources
        wgpuRenderPassEncoderSetPipeline(terminalScreenRenderContext.pass, pipeline);
        if (auto result = _gpuResourceBinder->bind(terminalScreenRenderContext.pass, 0); !result) {
            return Err<void>("Failed to bind", result);
        }

        // Draw quad
        WGPUBuffer vertexBuffer = _gpuResourceBinder->getQuadVertexBuffer();
        wgpuRenderPassEncoderSetVertexBuffer(terminalScreenRenderContext.pass, 0,
                                              vertexBuffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(terminalScreenRenderContext.pass, 6, 1, 0, 0);

        ytrace("TextGridLayer: rendered");
        return Ok();
    }

    bool isDirty() const override {
        return _terminalScreen->hasDamage();
    }

private:
    // Returns resource set for grid uniforms
    GpuResourceSet getGridUniformsResourceSet() const {
        GpuResourceSet rs;
        rs.shared = false;
        rs.name = "gridUniforms";
        rs.uniformSize = sizeof(_uniforms);
        rs.uniformWgslType = "GridUniforms";
        rs.uniformName = "grid";
        rs.uniformData = reinterpret_cast<const uint8_t*>(&_uniforms);
        rs.uniformDataSize = sizeof(_uniforms);
        return rs;
    }

    // Returns resource set for cell buffer
    GpuResourceSet getCellBufferResourceSet() const {
        GpuResourceSet rs;
        rs.shared = false;
        rs.name = "cells";
        uint32_t rows = _terminalScreen->getRows();
        uint32_t cols = _terminalScreen->getCols();
        rs.bufferSize = static_cast<size_t>(rows * cols) * sizeof(TextCell);
        rs.bufferWgslType = "array<Cell>";
        rs.bufferName = "cellBuffer";
        rs.bufferData = reinterpret_cast<const uint8_t*>(_terminalScreen->getCellData());
        rs.bufferDataSize = rs.bufferSize;
        rs.bufferReadonly = true;
        return rs;
    }

    // Combined for backward compatibility with GpuResourceBinder
    GpuResourceSet getGpuResourceSet() const {
        GpuResourceSet gpuResourceSet;
        gpuResourceSet.shared = false;
        gpuResourceSet.name = "textGridLayer";

        // Cells buffer
        uint32_t rows = _terminalScreen->getRows();
        uint32_t cols = _terminalScreen->getCols();
        gpuResourceSet.bufferSize = static_cast<size_t>(rows * cols) * sizeof(TextCell);
        gpuResourceSet.bufferData = reinterpret_cast<const uint8_t*>(_terminalScreen->getCellData());
        gpuResourceSet.bufferDataSize = gpuResourceSet.bufferSize;
        gpuResourceSet.bufferReadonly = true;

        // Uniforms (legacy packed approach for binder)
        gpuResourceSet.uniformFields = {
            {"projection", "mat4x4<f32>", 64},
            {"screenSize", "vec2<f32>", 8},
            {"cellSize", "vec2<f32>", 8},
            {"gridSize", "vec2<f32>", 8},
            {"cursorPos", "vec2<f32>", 8},
            {"cursorVisible", "f32", 4},
            {"cursorShape", "f32", 4},
            {"cursorBlink", "f32", 4},
            {"pixelRange", "f32", 4},
            {"scale", "f32", 4},
            {"defaultFg", "u32", 4},
            {"defaultBg", "u32", 4},
        };
        gpuResourceSet.uniformData = reinterpret_cast<const uint8_t*>(&_uniforms);
        gpuResourceSet.uniformDataSize = sizeof(_uniforms);

        return gpuResourceSet;
    }

    void updateUniforms(const TerminalScreenRenderContext& terminalScreenRenderContext) {
        auto& appGpuContext = terminalScreenRenderContext.terminalScreenContext.terminalContext.yettyContext.yettyGpuContext.appGpuContext;
        float width = static_cast<float>(appGpuContext.surfaceWidth);
        float height = static_cast<float>(appGpuContext.surfaceHeight);

        // Orthographic projection
        memset(&_uniforms, 0, sizeof(_uniforms));
        _uniforms.projection[0] = 2.0f / width;
        _uniforms.projection[5] = -2.0f / height;
        _uniforms.projection[10] = 1.0f;
        _uniforms.projection[12] = -1.0f;
        _uniforms.projection[13] = 1.0f;
        _uniforms.projection[15] = 1.0f;

        _uniforms.screenSize[0] = width;
        _uniforms.screenSize[1] = height;
        _uniforms.cellSize[0] = _terminalScreen->getCellWidth();
        _uniforms.cellSize[1] = _terminalScreen->getCellHeight();
        _uniforms.gridSize[0] = static_cast<float>(_terminalScreen->getCols());
        _uniforms.gridSize[1] = static_cast<float>(_terminalScreen->getRows());

        _uniforms.cursorPos[0] = static_cast<float>(_terminalScreen->getCursorCol());
        _uniforms.cursorPos[1] = static_cast<float>(_terminalScreen->getCursorRow());
        _uniforms.cursorVisible = _terminalScreen->isCursorVisible() ? 1.0f : 0.0f;
        _uniforms.cursorShape = static_cast<float>(_terminalScreen->getCursorShape());
        _uniforms.cursorBlink = _terminalScreen->isCursorBlink() ? 1.0f : 0.0f;
        _uniforms.scale = 1.0f;
        _uniforms.defaultFg = 0xFFFFFFFF;
        _uniforms.defaultBg = 0x000000FF;
    }

    void cleanup() {
        if (_gpuResourceBinder) {
            delete _gpuResourceBinder;
            _gpuResourceBinder = nullptr;
        } else {
            yerror("TextGridLayerImpl: _gpuResourceBinder was null");
        }
        _font = nullptr; // owned by TerminalScreen
        if (_shaderManager) {
            delete _shaderManager;
            _shaderManager = nullptr;
        } else {
            yerror("TextGridLayerImpl: _shaderManager was null");
        }
        if (_gpuAllocator) {
            delete _gpuAllocator;
            _gpuAllocator = nullptr;
        } else {
            yerror("TextGridLayerImpl: _gpuAllocator was null");
        }
    }

    //=========================================================================
    // Data
    //=========================================================================

    TerminalScreenRenderContext _terminalScreenRenderContext;
    GpuAllocator* _gpuAllocator = nullptr;
    ShaderManager* _shaderManager = nullptr;
    GpuResourceBinder* _gpuResourceBinder = nullptr;
    Font* _font = nullptr;
    bool _finalized = false;

    // Grid uniforms matching shader GridUniforms struct
    // WGSL struct size must be multiple of largest alignment (16 for mat4x4<f32>)
    // Fields = 120 bytes, padded to 128
    struct {
        float projection[16];    // mat4x4<f32>   64 bytes  offset 0
        float screenSize[2];     // vec2<f32>      8 bytes  offset 64
        float cellSize[2];       // vec2<f32>      8 bytes  offset 72
        float gridSize[2];       // vec2<f32>      8 bytes  offset 80
        float cursorPos[2];      // vec2<f32>      8 bytes  offset 88
        float cursorVisible;     // f32            4 bytes  offset 96
        float cursorShape;       // f32            4 bytes  offset 100
        float cursorBlink;       // f32            4 bytes  offset 104
        float scale;             // f32            4 bytes  offset 108
        uint32_t defaultFg;      // u32            4 bytes  offset 112
        uint32_t defaultBg;      // u32            4 bytes  offset 116
        uint32_t _pad0;          //                4 bytes  offset 120
        uint32_t _pad1;          //                4 bytes  offset 124
    } _uniforms = {};                           // total: 128 bytes
};

//=============================================================================
// Factory
//=============================================================================

Result<TextGridLayer*> TextGridLayer::createImpl(TerminalScreen* terminalScreen,
                                                  RenderableLayer* previousLayer,
                                                  const TerminalScreenRenderContext& terminalScreenRenderContext) {
    auto* layer = new TextGridLayerImpl();
    auto initResult = layer->init(terminalScreen, previousLayer, terminalScreenRenderContext);
    if (!initResult) {
        delete layer;
        return Err<TextGridLayer*>("Failed to init TextGridLayer", initResult);
    }
    return Ok(layer);
}

} // namespace yetty
