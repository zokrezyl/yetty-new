# WebGPU Concepts

This document explains WebGPU concepts used in yetty for terminal rendering.

## Core Objects (Creation Order)

### Instance
Entry point to WebGPU. Created once per application.
```
WGPUInstance = wgpuCreateInstance()
```
- Platform creates this in main()
- Passed to Yetty via AppContext

### Surface
Represents the window's drawable area. Platform-specific.
```
WGPUSurface = platform-specific creation from window handle
```
- GLFW: created via glfw3webgpu helper
- WebASM: created from HTML canvas
- Passed to Yetty via AppContext

### Adapter
Represents a physical GPU. Requested from instance.
```
wgpuInstanceRequestAdapter(instance, options) -> WGPUAdapter
```
- Options specify power preference, compatible surface
- Yetty requests this during init

### Device
Logical connection to GPU. All GPU operations go through device.
```
wgpuAdapterRequestDevice(adapter, descriptor) -> WGPUDevice
```
- Descriptor specifies required limits (buffer sizes, texture dimensions)
- Yetty requests this during init
- Passed to all GPU components via GPUContext

### Queue
Command submission queue. One per device.
```
WGPUQueue = wgpuDeviceGetQueue(device)
```
- Used to submit command buffers
- Used to write data to buffers/textures
- Passed to components via GPUContext

## Resource Objects

### Buffer
GPU memory for vertices, uniforms, storage data.
```cpp
WGPUBufferDescriptor desc = {
    .size = bytes,
    .usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst
};
WGPUBuffer = wgpuDeviceCreateBuffer(device, &desc)
```
Usage flags:
- `Vertex` - vertex data
- `Index` - index data
- `Uniform` - small constant data (uniforms)
- `Storage` - large read/write data (SSBOs)
- `CopyDst` - can receive data from CPU
- `CopySrc` - can be read back to CPU

### Texture
GPU image data.
```cpp
WGPUTextureDescriptor desc = {
    .size = {width, height, 1},
    .format = WGPUTextureFormat_RGBA8Unorm,
    .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst
};
WGPUTexture = wgpuDeviceCreateTexture(device, &desc)
```

### TextureView
How to interpret a texture (format, mip levels, array layers).
```cpp
WGPUTextureView = wgpuTextureCreateView(texture, &viewDesc)
```

### Sampler
How to sample a texture (filtering, wrapping).
```cpp
WGPUSamplerDescriptor desc = {
    .magFilter = WGPUFilterMode_Linear,
    .minFilter = WGPUFilterMode_Linear
};
WGPUSampler = wgpuDeviceCreateSampler(device, &desc)
```

## Shader Objects

### ShaderModule
Compiled shader code (WGSL).
```cpp
WGPUShaderModuleWGSLDescriptor wgslDesc = {
    .code = wgslSource
};
WGPUShaderModuleDescriptor desc = {
    .nextInChain = &wgslDesc.chain
};
WGPUShaderModule = wgpuDeviceCreateShaderModule(device, &desc)
```

## Bind Groups (Resource Binding)

### BindGroupLayout
Describes what resources a shader expects at each binding.
```cpp
WGPUBindGroupLayoutEntry entries[] = {
    {.binding = 0, .visibility = Fragment, .buffer.type = Uniform},
    {.binding = 1, .visibility = Fragment, .texture.sampleType = Float},
    {.binding = 2, .visibility = Fragment, .sampler.type = Filtering}
};
WGPUBindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &layoutDesc)
```

### BindGroup
Actual resources bound to a layout.
```cpp
WGPUBindGroupEntry entries[] = {
    {.binding = 0, .buffer = uniformBuffer, .size = sizeof(Uniforms)},
    {.binding = 1, .textureView = atlasView},
    {.binding = 2, .sampler = linearSampler}
};
WGPUBindGroup = wgpuDeviceCreateBindGroup(device, &desc)
```

### PipelineLayout
Combines multiple bind group layouts.
```cpp
WGPUBindGroupLayout layouts[] = {group0Layout, group1Layout};
WGPUPipelineLayout = wgpuDeviceCreatePipelineLayout(device, &desc)
```

In yetty shaders:
- Group 0: Shared resources (time, mouse, card data)
- Group 1: Per-terminal resources (uniforms, cells, fonts)

## Pipeline Objects

### RenderPipeline
Complete rendering configuration: shaders, vertex layout, blending, etc.
```cpp
WGPURenderPipelineDescriptor desc = {
    .layout = pipelineLayout,
    .vertex = {.module = shader, .entryPoint = "vs_main", ...},
    .fragment = {.module = shader, .entryPoint = "fs_main", ...},
    .primitive = {.topology = TriangleList},
    ...
};
WGPURenderPipeline = wgpuDeviceCreateRenderPipeline(device, &desc)
```

## Rendering (Per Frame)

### CommandEncoder
Records GPU commands.
```cpp
WGPUCommandEncoder = wgpuDeviceCreateCommandEncoder(device, nullptr)
```

### RenderPassEncoder
Records draw commands within a render pass.
```cpp
WGPURenderPassDescriptor passDesc = {
    .colorAttachments = {{.view = surfaceTextureView, .loadOp = Clear, ...}}
};
WGPURenderPassEncoder = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc)
```

### Drawing
```cpp
wgpuRenderPassEncoderSetPipeline(pass, pipeline);
wgpuRenderPassEncoderSetBindGroup(pass, 0, sharedBindGroup, 0, nullptr);
wgpuRenderPassEncoderSetBindGroup(pass, 1, terminalBindGroup, 0, nullptr);
wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quadBuffer, 0, size);
wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);  // 6 vertices = 2 triangles
wgpuRenderPassEncoderEnd(pass);
```

### Submit
```cpp
WGPUCommandBuffer cmdBuf = wgpuCommandEncoderFinish(encoder, nullptr);
wgpuQueueSubmit(queue, 1, &cmdBuf);
```

### Present
```cpp
wgpuSurfacePresent(surface);
```

## Yetty Resource Ownership

```
Platform (main)
  |
  +-- Instance, Surface (owned by platform)
  |
  +-- Yetty
        |
        +-- Adapter, Device, Queue (owned by Yetty)
        +-- GpuAllocator (tracks all GPU allocations)
        |
        +-- Terminal
              |
              +-- TerminalScreen
                    |
                    +-- ShaderManager (owned per-terminal)
                    |     +-- ShaderModule
                    |     +-- RenderPipeline
                    |     +-- PipelineLayout
                    |     +-- BindGroupLayouts (group 0, group 1)
                    |     +-- Shared BindGroup (group 0)
                    |     +-- Quad vertex buffer
                    |
                    +-- RasterFont
                    |     +-- Texture (atlas)
                    |     +-- TextureView
                    |     +-- Sampler
                    |     +-- Buffer (glyph metadata)
                    |
                    +-- Uniform buffer
                    +-- Cell buffer (SSBO)
                    +-- BindGroup (group 1)
```

## Data Flow (Rendering)

1. **CPU updates uniforms** (projection, cursor, screen size)
   ```cpp
   wgpuQueueWriteBuffer(queue, uniformBuffer, 0, &uniforms, size)
   ```

2. **CPU updates cells** (when terminal content changes)
   ```cpp
   wgpuQueueWriteBuffer(queue, cellBuffer, 0, cells, cellCount * sizeof(TextCell))
   ```

3. **GPU reads** uniforms and cells in fragment shader
4. **Fragment shader** samples font atlas, computes pixel colors
5. **Output** to surface texture, presented to window
