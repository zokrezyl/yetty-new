# Layered Rendering Architecture

## Overview

Terminal rendering is split into multiple layers, each rendering to its own cached texture. A final compositor blends all layer textures in a single pass.

**Key principles:**
- **Data vs Renderer separation**: Layer data (VTermGrid, Painter) is separate from layer renderers
- **Texture caching**: Unchanged layers skip re-rendering
- **Single blend pass**: Compositor blends all layers efficiently
- **Chain pattern**: Layers chain via `_previousLayer` pointer for ordering and dirty propagation

---

## Renaming Note

> **TODO**: Rename files to reflect the architecture:
> - `include/yetty/term/renderable-layer.hpp` → `include/yetty/term/layer-renderer.hpp`
> - `RenderableLayer` class → `LayerRenderer`
> - `TextGridLayer` → `TextGridRenderer`
> - Subclasses follow pattern: `*Renderer`

---

## Data Sources (owned by TerminalScreen)

```
TerminalScreen
├── VTermGrid              - cells, cursor, attributes
├── ScrollingYPaintData    - Painter for scrolling ypaint content
├── OverlayYPaintData      - Painter for static overlay content
├── SelectionData          - selection start/end positions
└── (future) SharedScrollback - Merkle tree with file refs
```

Data sources are **shared** - multiple renderers can reference the same data.

Example: `TextGridRenderer` and `ShaderGlyphRenderer` both read from `VTermGrid`.

---

## Renderer Chain

```
TerminalScreen._topRenderer
        │
        ▼
┌─────────────────────┐
│ OverlayYPaintRenderer │  ← renders static overlay
│   _previousLayer ───────┐
└─────────────────────┘   │
                          ▼
                 ┌─────────────────────┐
                 │ CursorRenderer      │  ← blinking cursor
                 │   _previousLayer ───────┐
                 └─────────────────────┘   │
                                           ▼
                                  ┌─────────────────────┐
                                  │ SelectionRenderer   │  ← selection highlight
                                  │   _previousLayer ───────┐
                                  └─────────────────────┘   │
                                                            ▼
                                                   ┌─────────────────────┐
                                                   │ ShaderGlyphRenderer │  ← animated glyphs
                                                   │   _previousLayer ───────┐
                                                   └─────────────────────┘   │
                                                                             ▼
                                                                    ┌─────────────────────┐
                                                                    │ScrollingYPaintRenderer│
                                                                    │   _previousLayer ───────┐
                                                                    └─────────────────────┘   │
                                                                                              ▼
                                                                                     ┌─────────────────────┐
                                                                                     │ TextGridRenderer    │  ← vterm cells
                                                                                     │   _previousLayer ───────┐
                                                                                     └─────────────────────┘   │
                                                                                                               ▼
                                                                                                     ┌─────────────────────┐
                                                                                                     │ BackgroundRenderer  │
                                                                                                     │   _previousLayer = nullptr
                                                                                                     └─────────────────────┘
```

---

## Composition Options

### Option A: Recursive Composition

Each layer blends its texture with the composited result from layers below.

```
Layer N render():
    compositeBelow = _previousLayer->render()   // recursive
    myTexture = renderSelf()
    return blend(myTexture, compositeBelow)
```

**Flow:**
```
Layer 3 calls Layer 2 calls Layer 1
                                 │
                                 ▼
                              render tex1
                         ◄────────────────
                    blend(tex2, tex1)
               ◄────────────────────────
          blend(tex3, result_2_1)
     ◄────────────────────────────────
   final result
```

**Pros:**
- Fits existing `_previousLayer` chain pattern exactly
- Each layer is self-contained

**Cons:**
- N blend passes (one per layer)
- N intermediate composite textures
- More GPU work

---

### Option B: Single Final Blend (CHOSEN)

Each layer renders to its own texture. A final compositor blends ALL textures in one pass.

```
Each layer render():
    _previousLayer->render()    // chain for ordering only
    if (isDirty()) {
        renderToOwnTexture()
    }

Compositor:
    blend(tex0, tex1, tex2, ..., texN)  // single pass
```

**Flow:**
```
render() chain (ordering + dirty check):
    Layer 3 → Layer 2 → Layer 1 → Layer 0
    
Each layer renders to own texture (if dirty):
    Layer 0 ──► tex0
    Layer 1 ──► tex1
    Layer 2 ──► tex2
    Layer 3 ──► tex3
    
Compositor (single pass):
    ┌────────────────────────────────┐
    │ sample tex0, tex1, tex2, tex3  │
    │ blend all together             │
    │ output final color             │
    └────────────────────────────────┘
```

**Pros:**
- Efficient: 1 blend pass regardless of layer count
- GPU friendly: single full-screen quad
- Compositor can apply global effects
- Chain pattern preserved for ordering

**Cons:**
- Requires separate compositor pass
- All layer textures must be bound simultaneously

---

## LayerRenderer Interface

```cpp
class LayerRenderer : public core::FactoryObject<LayerRenderer> {
public:
    virtual ~LayerRenderer() = default;

    // Chain to render layers in order, each to own texture
    virtual Result<void> render(const LayerRenderContext& ctx) = 0;

    // Is this layer's cached texture stale?
    virtual bool isDirty() const = 0;
    virtual void markDirty() = 0;

    // Get cached texture for compositor
    virtual WGPUTextureView getOutputTexture() const = 0;

    // Blend settings for compositor
    virtual BlendMode blendMode() const { return BlendMode::Normal; }
    virtual float opacity() const { return 1.0f; }

protected:
    LayerRenderer* _previousLayer = nullptr;
    TerminalScreen* _terminalScreen = nullptr;
    
    WGPUTexture _outputTexture = nullptr;
    WGPUTextureView _outputTextureView = nullptr;
    bool _dirty = true;
};
```

---

## Render Implementation Pattern

```cpp
Result<void> SomeRenderer::render(const LayerRenderContext& ctx) {
    // 1. Chain down (for ordering and dirty propagation)
    if (_previousLayer) {
        if (auto res = _previousLayer->render(ctx); !res) {
            return res;
        }
    }

    // 2. Skip if not dirty (use cached texture)
    if (!_dirty) {
        return Ok();
    }

    // 3. Render to own texture
    // ... GPU work here ...

    _dirty = false;
    return Ok();
}
```

---

## Compositor

```cpp
class LayerCompositor {
public:
    Result<void> composite(
        WGPURenderPassEncoder pass,
        const std::vector<LayerRenderer*>& layers
    );
};
```

**Compositor shader (pseudo-WGSL):**

```wgsl
@group(0) @binding(0) var layers: texture_2d_array<f32>;
@group(0) @binding(1) var layerSampler: sampler;

struct LayerInfo {
    blendMode: u32,
    opacity: f32,
    _pad: vec2<f32>,
};
@group(0) @binding(2) var<uniform> layerInfos: array<LayerInfo, 8>;
@group(0) @binding(3) var<uniform> layerCount: u32;

fn blend(below: vec4<f32>, above: vec4<f32>, mode: u32, opacity: f32) -> vec4<f32> {
    let a = above.a * opacity;
    switch (mode) {
        case 0u: { // Normal
            return vec4(mix(below.rgb, above.rgb, a), max(below.a, a));
        }
        case 1u: { // Additive
            return vec4(below.rgb + above.rgb * a, max(below.a, a));
        }
        case 2u: { // Multiply
            return vec4(mix(below.rgb, below.rgb * above.rgb, a), max(below.a, a));
        }
        default: {
            return below;
        }
    }
}

@fragment
fn fs_main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    var result = textureSample(layers, layerSampler, uv, 0);  // bottom layer
    
    for (var i = 1u; i < layerCount; i++) {
        let layer = textureSample(layers, layerSampler, uv, i32(i));
        let info = layerInfos[i];
        result = blend(result, layer, info.blendMode, info.opacity);
    }
    
    return result;
}
```

---

## Dirty Tracking

| Event | Layers Marked Dirty |
|-------|---------------------|
| PTY data received | TextGridRenderer, ShaderGlyphRenderer, CursorRenderer |
| YPaint OSC received | ScrollingYPaintRenderer or OverlayYPaintRenderer |
| Selection changed | SelectionRenderer |
| Cursor blink tick | CursorRenderer |
| Animation frame | ShaderGlyphRenderer |
| Window resize | ALL layers |

---

## Frame Render Flow

```cpp
void TerminalScreen::renderFrame() {
    // 1. Process input (PTY data, OSC, etc.) - marks layers dirty
    processInput();

    // 2. Chain render - each layer renders to own texture if dirty
    if (auto res = _topRenderer->render(ctx); !res) {
        yerror("Layer render failed: {}", res.error().message());
        return;
    }

    // 3. Collect layer textures
    std::vector<LayerRenderer*> layers;
    for (auto* r = _topRenderer; r; r = r->previousLayer()) {
        layers.push_back(r);
    }
    std::reverse(layers.begin(), layers.end());  // bottom-to-top order

    // 4. Compositor blends all layer textures
    _compositor->composite(pass, layers);
}
```

---

## Memory Considerations

Each layer texture: `width * height * 4 bytes` (RGBA8)

For 1920x1080 with 8 layers: **63 MB**

Mitigations:
- Layers that rarely change can share textures
- Simple layers (cursor, selection) can use smaller textures
- Consider RGBA4 for layers that don't need full precision

---

## Implementation Phases

1. **Rename classes** (RenderableLayer → LayerRenderer, etc.)
2. **Add per-layer render targets** to LayerRenderer base
3. **Implement LayerCompositor**
4. **Refactor TextGridRenderer** to render to own texture
5. **Add BackgroundRenderer** (simplest layer)
6. **Add ScrollingYPaintRenderer** with Painter integration
7. **Add remaining layers** (Selection, Cursor, ShaderGlyph, Overlay)
8. **Wire up TerminalScreen** to use compositor

---

## Future: Scrollback Integration

All layers should be able to write to a shared scrollback (out of scope for now).

Planned approach:
- Merkle tree structure
- Large objects (ypaint primitives) stored in separate files
- Scrollback references objects by hash
- Efficient deduplication and persistence
