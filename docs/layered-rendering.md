# Layered Rendering Architecture

## Overview

Terminal rendering is split into multiple layers (text, ypaint, etc.), each rendering to its own texture. A blender composites all layer textures to a render target.

**Key principles:**
- **Simple orchestration** - terminal.c iterates layers and calls blender
- **Complex logic in render/** - Rendering and blending logic in `src/yetty/render/`
- **Blender owns target** - Target is encapsulated, can be changed at runtime
- **Layer independence** - Each layer has its own pipeline and intermediate texture

---

## Architecture

```
terminal.c (simple orchestrator)
    │
    ├── for each layer:
    │       renderer->ops->render(layer) → rendered_layer
    │
    └── blender->ops->blend(rendered_layers[], count)
                │
                ▼
            render_target (owned by blender)
                │
                ▼
            screen / VNC / other
```

---

## Components (src/yetty/render/)

### 1. render_target

Abstraction for render output. Owned by blender.

```c
struct yetty_render_target_ops {
    void (*destroy)(struct yetty_render_target *self);
    WGPUTextureView (*acquire)(struct yetty_render_target *self);
    void (*present)(struct yetty_render_target *self);
    uint32_t (*get_width)(const struct yetty_render_target *self);
    uint32_t (*get_height)(const struct yetty_render_target *self);
};
```

Implementations:
- **surface_target** - Local window (WGPUSurface)
- **vnc_target** - VNC server buffer (future)
- **multi_target** - Multiple targets simultaneously (future)

### 2. rendered_layer

Opaque handle for a layer rendered to texture.

```c
struct yetty_render_rendered_layer_ops {
    void (*release)(struct yetty_render_rendered_layer *self);
    WGPUTextureView (*get_view)(const struct yetty_render_rendered_layer *self);
    uint32_t (*get_width)(const struct yetty_render_rendered_layer *self);
    uint32_t (*get_height)(const struct yetty_render_rendered_layer *self);
};
```

### 3. layer_renderer

Renders a terminal_layer to texture.

```c
struct yetty_render_layer_renderer_ops {
    void (*destroy)(struct yetty_render_layer_renderer *self);

    struct yetty_render_rendered_layer_result (*render)(
        struct yetty_render_layer_renderer *self,
        struct yetty_term_terminal_layer *layer);
};
```

Responsibilities:
- Creates/manages per-layer binder
- Creates/manages intermediate texture
- Handles resize
- Returns rendered_layer handle

### 4. blender

Composites layers to target. **Owns the render target.**

```c
struct yetty_render_blender_ops {
    void (*destroy)(struct yetty_render_blender *self);

    struct yetty_core_void_result (*blend)(
        struct yetty_render_blender *self,
        struct yetty_render_rendered_layer **layers,
        size_t layer_count);

    void (*set_target)(
        struct yetty_render_blender *self,
        struct yetty_render_target *target);
};
```

Target ownership:
- Blender takes ownership of target
- `set_target()` destroys old target, takes new one
- Enables runtime target switching (screen → VNC)

---

## terminal.c Integration

```c
struct yetty_term_terminal {
    /* layers */
    struct yetty_term_terminal_layer *layers[MAX_LAYERS];
    size_t layer_count;

    /* rendering */
    struct yetty_render_layer_renderer *renderer;
    struct yetty_render_blender *blender;
};

static struct yetty_core_void_result terminal_render_frame(
    struct yetty_term_terminal *terminal)
{
    struct yetty_render_rendered_layer *rendered[MAX_LAYERS];
    size_t rendered_count = 0;

    /* Render each layer to texture */
    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_render_rendered_layer_result res =
            terminal->renderer->ops->render(
                terminal->renderer,
                terminal->layers[i]);
        if (YETTY_IS_OK(res)) {
            rendered[rendered_count++] = res.value;
        }
    }

    /* Blend to target */
    return terminal->blender->ops->blend(
        terminal->blender,
        rendered,
        rendered_count);
}
```

---

## Data Flow

```
terminal_layer (text, ypaint, etc.)
       │
       │ layer->ops->get_gpu_resource_set()
       ▼
gpu_resource_set (buffers, textures, uniforms, shader)
       │
       │ layer_renderer creates binder, pipeline
       ▼
intermediate texture
       │
       │ returned as rendered_layer
       ▼
blender collects all rendered_layers
       │
       │ alpha-over compositing
       ▼
render_target (screen, VNC, etc.)
```

---

## Blend Shader (blend.wgsl)

```wgsl
@group(0) @binding(0) var layer_textures: binding_array<texture_2d<f32>>;
@group(0) @binding(1) var layer_sampler: sampler;
@group(0) @binding(2) var<uniform> layer_count: u32;

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let uv = pos.xy / vec2<f32>(target_size);

    // Start with bottom layer
    var result = textureSample(layer_textures[0], layer_sampler, uv);

    // Blend subsequent layers (alpha-over, premultiplied)
    for (var i = 1u; i < layer_count; i++) {
        let layer = textureSample(layer_textures[i], layer_sampler, uv);
        result = vec4(
            result.rgb * (1.0 - layer.a) + layer.rgb,
            result.a * (1.0 - layer.a) + layer.a
        );
    }

    return result;
}
```

---

## Runtime Target Switching

```c
/* Switch from screen to VNC */
struct yetty_render_target *vnc_target =
    yetty_render_target_vnc_create(vnc_context);

terminal->blender->ops->set_target(terminal->blender, vnc_target);
/* Old target destroyed, VNC target now owned by blender */

/* Subsequent renders go to VNC */
terminal_render_frame(terminal);
```

---

## File Structure

```
src/yetty/render/
├── render-target.h         # target interface
├── render-target-surface.c # WGPUSurface implementation
├── rendered-layer.h        # rendered layer handle
├── rendered-layer.c
├── layer-renderer.h        # layer → texture renderer
├── layer-renderer.c
├── blender.h               # compositor, owns target
├── blender.c
└── blend.wgsl              # compositing shader
```

---

## Future Extensions (Out of Scope)

- **VNC target** - Render to VNC server buffer
- **Multi-target** - Render to multiple targets simultaneously
- **Layer effects** - Per-layer post-processing
- **Dirty regions** - Partial re-rendering
