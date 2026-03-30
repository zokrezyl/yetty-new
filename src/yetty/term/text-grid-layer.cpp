#include <yetty/term/text-grid-layer.hpp>
#include <yetty/term/terminal-screen.hpp>
#include <yetty/config.hpp>
#include <yetty/gpu-resource-binder.hpp>
#include <yetty/gpu-allocator.hpp>
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
        auto allocatorResult = GpuAllocator::create(yettyGpuContext.device);
        if (!allocatorResult) {
            return Err<void>("Failed to create GpuAllocator", allocatorResult);
        }
        _allocator = *allocatorResult;

        // Create ShaderManager (don't compile yet - wait for GpuResourceSets)
        std::string shadersDir = yettyContext.appContext.config->get<std::string>("paths/shaders", "");
        auto shaderManagerResult = ShaderManager::create(
            yettyGpuContext,
            _allocator,
            shadersDir);
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
        auto binderResult = GpuResourceBinder::create(
            yettyGpuContext,
            _allocator);
        if (!binderResult) {
            return Err<void>("Failed to create GpuResourceBinder", binderResult);
        }
        _binder = *binderResult;

        ydebug("TextGridLayer initialized");
        return Ok();
    }

    //=========================================================================
    // RenderableLayer interface
    //=========================================================================

    void render(const TerminalScreenRenderContext& terminalScreenRenderContext) override {
        // Compile shaders on first render (after GpuResourceSets are known)
        if (!_shadersCompiled) {
            if (auto compileResult = _shaderManager->compile(); !compileResult) {
                yerror("TextGridLayer: failed to compile shaders: {}", error_msg(compileResult));
                return;
            }
            _pipeline = _shaderManager->getGridPipeline();
            _vertexBuffer = _shaderManager->getQuadVertexBuffer();
            _shadersCompiled = true;
        }

        if (!_pipeline) {
            yerror("TextGridLayer: pipeline not ready");
            return;
        }

        // Update uniforms from terminal state
        updateUniforms(terminalScreenRenderContext);

        // Pass GpuResourceSets to binder
        _binder->addGpuResourceSet(getGpuResourceSet());
        _binder->addGpuResourceSet(_font->getGpuResourceSet());

        // Set pipeline and bind
        wgpuRenderPassEncoderSetPipeline(terminalScreenRenderContext.pass, _pipeline);
        wgpuRenderPassEncoderSetBindGroup(terminalScreenRenderContext.pass, 0,
                                           _shaderManager->getSharedBindGroup(), 0, nullptr);
        _binder->bind(terminalScreenRenderContext.pass, 1);

        // Draw
        wgpuRenderPassEncoderSetVertexBuffer(terminalScreenRenderContext.pass, 0,
                                              _vertexBuffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(terminalScreenRenderContext.pass, 6, 1, 0, 0);

        ytrace("TextGridLayer: rendered");
    }

    bool isDirty() const override {
        return _terminalScreen->hasDamage();
    }

private:
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

        // Uniforms
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
        float width = static_cast<float>(appGpuContext.windowWidth);
        float height = static_cast<float>(appGpuContext.windowHeight);

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

        _uniforms.pixelRange = 4.0f;
        _uniforms.scale = 1.0f;
        _uniforms.defaultFg = 0xFFFFFFFF;
        _uniforms.defaultBg = 0x000000FF;
    }

    void cleanup() {
        if (_binder) { delete _binder; _binder = nullptr; }
        _font = nullptr; // owned by TerminalScreen
        if (_shaderManager) { delete _shaderManager; _shaderManager = nullptr; }
        if (_allocator) { delete _allocator; _allocator = nullptr; }
    }

    //=========================================================================
    // Data
    //=========================================================================

    TerminalScreenRenderContext _terminalScreenRenderContext;
    GpuAllocator* _allocator = nullptr;
    ShaderManager* _shaderManager = nullptr;
    Font* _font = nullptr;

    WGPURenderPipeline _pipeline = nullptr;
    WGPUBuffer _vertexBuffer = nullptr;
    bool _shadersCompiled = false;

    // Packed uniforms struct matching uniformFields
    struct {
        float projection[16];
        float screenSize[2];
        float cellSize[2];
        float gridSize[2];
        float cursorPos[2];
        float cursorVisible;
        float cursorShape;
        float cursorBlink;
        float pixelRange;
        float scale;
        uint32_t defaultFg;
        uint32_t defaultBg;
    } _uniforms = {};
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
