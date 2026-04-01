# GPU Resource Binding

## Architecture

```
TerminalScreen (orchestrates layers)
  └── TextGridLayer (has GpuResourceBinder)
        ├── Layer's GpuResourceSet: uniforms + cells buffer
        └── Font's GpuResourceSet: texture + sampler + glyph buffer
  └── OverlayLayer (has GpuResourceBinder)
        ├── Layer's GpuResourceSet: uniforms + overlay data
        └── Other objects' GpuResourceSets
```

- **TerminalScreen**: orchestrates render layers, does NOT provide GpuResourceSet
- **RenderLayer**: provides its own GpuResourceSet (uniforms + layer-specific buffers) and owns a GpuResourceBinder
- **Objects** (Font, etc.): provide their GpuResourceSets to the layer's binder

## GpuResourceSet

Struct with resource descriptions + CPU data pointers:

```cpp
struct GpuResourceSet {
    bool shared;                      // bind group 0 or 1
    std::string name;

    // Texture
    uint32_t textureWidth, textureHeight;
    WGPUTextureFormat textureFormat;
    const uint8_t* textureData;
    size_t textureDataSize;

    // Sampler
    WGPUFilterMode samplerFilter;

    // Buffer (storage)
    size_t bufferSize;
    const uint8_t* bufferData;
    size_t bufferDataSize;
    bool bufferReadonly;

    // Uniforms
    std::vector<UniformField> uniformFields;
    const uint8_t* uniformData;
    size_t uniformDataSize;
};
```

## RenderLayer Pattern

Each RenderLayer:
1. Owns a GpuResourceBinder
2. Provides its own GpuResourceSet (uniforms + layer data like cells)
3. Owns objects (Font, etc.) that provide GpuResourceSets

### TextGridLayer Example

```cpp
class TextGridLayer {
    TerminalScreen* _terminalScreen;  // provides cells data, cursor state (NO GPU knowledge)
    GpuResourceBinder* _binder;
    Font* _font;
    GridUniforms _uniforms;  // projection, screenSize, cellSize, cursor, etc.

    GpuResourceSet getGpuResourceSet() const {
        GpuResourceSet res;
        res.name = "textGridLayer";

        // Cells buffer - pulled from TerminalScreen
        size_t cellCount = _terminalScreen->getRows() * _terminalScreen->getCols();
        res.bufferSize = cellCount * sizeof(TextCell);
        res.bufferData = (const uint8_t*)_terminalScreen->getCellData();
        res.bufferDataSize = res.bufferSize;
        res.bufferReadonly = true;

        // Uniforms - layer's own struct
        res.uniformFields = {
            {"projection", "mat4x4<f32>", 64},
            {"screenSize", "vec2<f32>", 8},
            {"cellSize", "vec2<f32>", 8},
            {"gridSize", "vec2<f32>", 8},
            {"cursorPos", "vec2<f32>", 8},
            // ... etc
        };
        res.uniformData = (const uint8_t*)&_uniforms;
        res.uniformDataSize = sizeof(_uniforms);

        return res;
    }

    void render(WGPURenderPassEncoder pass) {
        // Update uniforms from terminal state
        _uniforms.cursorPos = {_terminalScreen->getCursorCol(), _terminalScreen->getCursorRow()};
        // ...

        // Pass GpuResourceSets to binder every frame
        _binder->addGpuResourceSet(getGpuResourceSet());
        _binder->addGpuResourceSet(_font->getGpuResourceSet());

        // Bind and draw
        _binder->bind(pass, 1);
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
    }
};
```

## GpuResourceBinder

Two methods:

```cpp
class GpuResourceBinder {
    // Called every frame - creates GPU resources on first call, uploads from data pointers
    void addGpuResourceSet(const GpuResourceSet& gpuResourceSet);

    // Called every frame after all addGpuResourceSet - binds to render pass
    void bind(WGPURenderPassEncoder pass, uint32_t groupIndex);
};
```

## Frame Flow

```cpp
void TextGridLayer::render(WGPURenderPassEncoder pass) {
    // Update layer uniforms
    _uniforms.projection = ...;
    _uniforms.cursorPos = ...;

    // Pass current data to binder
    _binder->addGpuResourceSet(getGpuResourceSet());
    _binder->addGpuResourceSet(_font->getGpuResourceSet());

    // Bind to render pass
    _binder->bind(pass, 1);

    // Draw
    wgpuRenderPassEncoderDraw(pass, ...);
}
```

**First frame** (per resource name): GpuResourceBinder creates GPU resources
**Every frame**: GpuResourceBinder uploads from data pointers, binds

## Bind Group Layout

GpuResourceBinder creates its own bind group layout based on added GpuResourceSets. Binding indices assigned sequentially per resource type (texture, sampler, buffer, uniform).
