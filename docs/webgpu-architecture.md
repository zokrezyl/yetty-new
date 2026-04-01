# WebGPU Architecture

## Ownership

### Platform (main thread only)
Creates ONLY platform-specific objects that require main thread:
```
WGPUInstance  - entry point, platform-specific creation
WGPUSurface   - window handle, platform-specific
```
These are passed to Yetty via AppContext.

### Yetty (render thread)
Creates and owns the GPU connection:
```
WGPUAdapter        - requested from instance
WGPUDevice         - requested from adapter
WGPUQueue          - obtained from device
GpuAllocator       - tracks all GPU allocations
```

### Views (owned by Yetty)
Each view (TerminalView, etc.) owns its rendering state:
```
ShaderManager      - pipeline, bind group layouts
Buffers            - uniforms, cells
BindGroups         - per-view resources
RenderTarget       - where to render (surface or texture)
```

### Shared Resources (owned by Yetty, used by views)
Large resources shared across views:
```
MsdfFont           - large atlas, shared by all terminal views
SharedUniforms     - time, mouse position
```

## Object Hierarchy

```
Platform (main.cpp)
│
├── Instance       ─┐ Platform-specific
├── Surface        ─┘ Passed to Yetty
│
└── Yetty (render thread)
    │
    ├── Adapter
    ├── Device
    ├── Queue
    ├── GpuAllocator
    │
    ├── Shared Resources
    │   ├── MsdfFont (large atlas, one per app)
    │   ├── SharedUniformBuffer
    │   └── SharedBindGroup (group 0)
    │
    └── Views
        │
        ├── TerminalView
        │   ├── ShaderManager
        │   ├── RasterFont (per-view, or use shared MsdfFont)
        │   ├── UniformBuffer
        │   ├── CellBuffer
        │   ├── BindGroup (group 1)
        │   └── RenderTarget
        │
        └── TerminalView (another terminal)
            └── ...
```

## One Queue, Multiple Views

WebGPU has ONE queue per device. All rendering is serialized through this queue.

### How it works:

```cpp
void Yetty::renderFrame() {
    // 1. Get surface texture for this frame
    WGPUTexture surfaceTexture = wgpuSurfaceGetCurrentTexture(surface);
    WGPUTextureView surfaceView = wgpuTextureCreateView(surfaceTexture, nullptr);

    // 2. Update shared uniforms (time, mouse)
    updateSharedUniforms();
    wgpuQueueWriteBuffer(queue, sharedUniformBuffer, 0, &sharedUniforms, size);

    // 3. Each view records its commands into ONE encoder
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);

    for (View* view : views) {
        // Each view gets the same encoder
        // Views render to their own region or layer
        view->render(encoder, surfaceView);
    }

    // 4. Submit all commands at once
    WGPUCommandBuffer cmdBuf = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &cmdBuf);

    // 5. Present
    wgpuSurfacePresent(surface);
}
```

### View rendering:

```cpp
void TerminalView::render(WGPUCommandEncoder encoder, WGPUTextureView target) {
    // Update per-view data
    updateUniforms();
    wgpuQueueWriteBuffer(queue, uniformBuffer, 0, &uniforms, size);

    if (hasDamage()) {
        wgpuQueueWriteBuffer(queue, cellBuffer, 0, cells, cellCount * sizeof(TextCell));
        clearDamage();
    }

    // Begin render pass for this view's region
    WGPURenderPassDescriptor passDesc = {};
    passDesc.colorAttachments[0].view = target;
    passDesc.colorAttachments[0].loadOp = WGPULoadOp_Load;  // Don't clear, preserve other views

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);

    // Set scissor rect for this view's region (if multiple views on screen)
    wgpuRenderPassEncoderSetScissorRect(pass, x, y, width, height);

    // Draw
    wgpuRenderPassEncoderSetPipeline(pass, shaderManager->getPipeline());
    wgpuRenderPassEncoderSetBindGroup(pass, 0, yetty->getSharedBindGroup(), 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quadBuffer, 0, 48);
    wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

    wgpuRenderPassEncoderEnd(pass);
}
```

## Bind Groups

### Group 0: Shared (owned by Yetty)
Resources shared across all views:
```
binding 0: SharedUniforms (time, mouse, screen size)
binding 1: MsdfFont atlas texture
binding 2: MsdfFont sampler
binding 3: MsdfFont glyph metadata
```
One bind group, created by Yetty, used by all views.

### Group 1: Per-View (owned by each View)
Resources specific to one terminal:
```
binding 0: ViewUniforms (projection, cursor, cell size)
binding 1: CellBuffer (terminal content)
binding 2: RasterFont atlas (if not using shared MSDF)
binding 3: RasterFont sampler
binding 4: RasterFont metadata
```
Each view creates its own bind group.

## Offscreen Rendering (VNC Mode)

View renders to texture instead of surface:

```cpp
class TerminalView {
    WGPUTexture offscreenTexture;      // render target
    WGPUBuffer readbackBuffer;          // for CPU access

    void render(WGPUCommandEncoder encoder, WGPUTextureView target) {
        // If VNC mode, use offscreen texture instead of surface
        WGPUTextureView actualTarget = vncMode ? offscreenTextureView : target;

        // ... render as usual ...

        // If VNC mode, copy to readback buffer
        if (vncMode) {
            wgpuCommandEncoderCopyTextureToBuffer(encoder,
                &offscreenTexture, &readbackBuffer, &extent);
        }
    }

    void sendToVnc() {
        // Map readback buffer, send pixels
        wgpuBufferMapAsync(readbackBuffer, ...);
    }
};
```

## Summary

| Object | Created By | Thread | Shared? |
|--------|------------|--------|---------|
| Instance | Platform | Main | - |
| Surface | Platform | Main | - |
| Adapter | Yetty | Render | - |
| Device | Yetty | Render | - |
| Queue | Yetty | Render | Yes, one per app |
| GpuAllocator | Yetty | Render | Yes |
| MsdfFont | Yetty | Render | Yes |
| SharedBindGroup | Yetty | Render | Yes (group 0) |
| ShaderManager | View | Render | No, per view |
| ViewBindGroup | View | Render | No, per view |
| Buffers | View | Render | No, per view |

## Current State vs This Model

Current code already follows this mostly:
- Platform creates Instance, Surface ✓
- Yetty creates Adapter, Device, Queue ✓
- GpuAllocator exists ✓

Missing:
- Clear View abstraction
- Shared bind group (group 0) owned by Yetty
- Per-view bind group (group 1) owned by view
- Render coordination (who calls render, when)
