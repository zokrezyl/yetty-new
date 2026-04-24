/*
 * tile-diff.c - GPU-accelerated per-tile frame diff.
 *
 * Extracted from src/yetty/yvnc/vnc-server.c so the diff + readback pipeline
 * can be reused by multiple sinks (today: VNC wire encoder; soon: X11-tile
 * render target for fast VNC-over-X11 presentation).
 */

#include <yetty/yrender-utils/tile-diff.h>
#include <yetty/yplatform/ycoroutine.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

/* Compute shader: compares currTex vs prevTex, sets dirtyFlags[tileIdx]=1
 * if any pixel in that tile differs. One workgroup per tile. */
static const char *DIFF_SHADER =
    "@group(0) @binding(0) var currTex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var prevTex: texture_2d<f32>;\n"
    "@group(0) @binding(2) var<storage, read_write> dirtyFlags: array<u32>;\n"
    "\n"
    "const TILE_SIZE: u32 = 64;\n"
    "const PIXELS_PER_THREAD: u32 = 8;\n"
    "\n"
    "@compute @workgroup_size(8, 8)\n"
    "fn main(@builtin(local_invocation_id) lid: vec3<u32>,\n"
    "        @builtin(workgroup_id) wgid: vec3<u32>) {\n"
    "    let dims = textureDimensions(currTex);\n"
    "    let tilesX = (dims.x + TILE_SIZE - 1u) / TILE_SIZE;\n"
    "    let tileIdx = wgid.y * tilesX + wgid.x;\n"
    "    let tileStartX = wgid.x * TILE_SIZE;\n"
    "    let tileStartY = wgid.y * TILE_SIZE;\n"
    "\n"
    "    let regionStartX = tileStartX + lid.x * PIXELS_PER_THREAD;\n"
    "    let regionStartY = tileStartY + lid.y * PIXELS_PER_THREAD;\n"
    "\n"
    "    for (var dy: u32 = 0u; dy < PIXELS_PER_THREAD; dy++) {\n"
    "        for (var dx: u32 = 0u; dx < PIXELS_PER_THREAD; dx++) {\n"
    "            let px = regionStartX + dx;\n"
    "            let py = regionStartY + dy;\n"
    "\n"
    "            if (px >= dims.x || py >= dims.y) {\n"
    "                continue;\n"
    "            }\n"
    "\n"
    "            let curr = textureLoad(currTex, vec2<u32>(px, py), 0);\n"
    "            let prev = textureLoad(prevTex, vec2<u32>(px, py), 0);\n"
    "\n"
    "            if (any(curr != prev)) {\n"
    "                dirtyFlags[tileIdx] = 1u;\n"
    "                return;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "}\n";

struct yetty_yrender_utils_tile_diff_engine {
    WGPUDevice device;
    WGPUQueue queue;
    struct yplatform_wgpu *wgpu;

    uint32_t tile_size;

    /* Diff pipeline — built lazily on first submit. */
    WGPUComputePipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;

    /* Per-size GPU resources, reallocated when dimensions change. */
    uint32_t last_width;
    uint32_t last_height;
    uint32_t tiles_x;
    uint32_t tiles_y;
    WGPUTexture prev_texture;
    WGPUBuffer dirty_flags_buffer;    /* GPU storage (compute shader writes) */
    WGPUBuffer dirty_flags_readback;  /* mappable, size = num_tiles * u32 */
    WGPUBuffer tile_readback_buffer;  /* mappable, holds full frame pixels */
    uint32_t tile_readback_buffer_size;

    /* CPU-side scratch, lives across calls to avoid reallocs. */
    uint8_t *dirty_bitmap;            /* num_tiles bytes, 0/1 per tile */
    uint32_t dirty_bitmap_len;

    /* Control flags. */
    bool prev_has_content;
    bool force_full_frame;
    bool always_full_frame;
};

/* Per-submit coroutine args. Freed inside the coro. */
struct submit_args {
    struct yetty_yrender_utils_tile_diff_engine *eng;
    WGPUTexture texture;
    uint32_t width;
    uint32_t height;
    yetty_yrender_utils_tile_diff_sink_fn sink_fn;
    void *sink_ctx;
};

static struct yetty_ycore_void_result
create_pipeline(struct yetty_yrender_utils_tile_diff_engine *eng)
{
    WGPUShaderSourceWGSL wgsl = {0};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = (WGPUStringView){.data = DIFF_SHADER, .length = strlen(DIFF_SHADER)};

    WGPUShaderModuleDescriptor shader_desc = {0};
    shader_desc.nextInChain = (WGPUChainedStruct *)&wgsl;

    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(eng->device, &shader_desc);
    if (!shader)
        return YETTY_ERR(yetty_ycore_void, "failed to create diff shader");

    WGPUBindGroupLayoutEntry entries[3] = {0};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Compute;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Compute;
    entries[2].buffer.type = WGPUBufferBindingType_Storage;

    WGPUBindGroupLayoutDescriptor bgl_desc = {0};
    bgl_desc.entryCount = 3;
    bgl_desc.entries = entries;
    eng->bind_group_layout = wgpuDeviceCreateBindGroupLayout(eng->device, &bgl_desc);

    WGPUPipelineLayoutDescriptor pl_desc = {0};
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &eng->bind_group_layout;
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(eng->device, &pl_desc);

    WGPUComputePipelineDescriptor cp_desc = {0};
    cp_desc.layout = layout;
    cp_desc.compute.module = shader;
    cp_desc.compute.entryPoint = (WGPUStringView){.data = "main", .length = 4};

    eng->pipeline = wgpuDeviceCreateComputePipeline(eng->device, &cp_desc);

    wgpuShaderModuleRelease(shader);
    wgpuPipelineLayoutRelease(layout);

    if (!eng->pipeline)
        return YETTY_ERR(yetty_ycore_void, "failed to create diff pipeline");

    return YETTY_OK_VOID();
}

/*
 * Allocate/resize the per-frame GPU resources. Called at the top of each
 * submit; no-op when dimensions match the cached ones. Forces a full frame
 * on resize since prev_texture is now stale.
 */
static struct yetty_ycore_void_result
ensure_resources(struct yetty_yrender_utils_tile_diff_engine *eng,
                 uint32_t width, uint32_t height)
{
    if (eng->last_width == width && eng->last_height == height && eng->prev_texture)
        return YETTY_OK_VOID();

    eng->force_full_frame = true;
    eng->prev_has_content = false;

    if (eng->prev_texture) {
        wgpuTextureRelease(eng->prev_texture);
        eng->prev_texture = NULL;
    }
    if (eng->dirty_flags_buffer) {
        wgpuBufferRelease(eng->dirty_flags_buffer);
        eng->dirty_flags_buffer = NULL;
    }
    if (eng->dirty_flags_readback) {
        wgpuBufferRelease(eng->dirty_flags_readback);
        eng->dirty_flags_readback = NULL;
    }
    if (eng->tile_readback_buffer) {
        wgpuBufferRelease(eng->tile_readback_buffer);
        eng->tile_readback_buffer = NULL;
        eng->tile_readback_buffer_size = 0;
    }

    eng->last_width = width;
    eng->last_height = height;
    eng->tiles_x = (width + eng->tile_size - 1) / eng->tile_size;
    eng->tiles_y = (height + eng->tile_size - 1) / eng->tile_size;

    uint32_t num_tiles = eng->tiles_x * eng->tiles_y;

    if (eng->dirty_bitmap_len < num_tiles) {
        free(eng->dirty_bitmap);
        eng->dirty_bitmap = calloc(num_tiles, 1);
        if (!eng->dirty_bitmap)
            return YETTY_ERR(yetty_ycore_void, "failed to allocate dirty bitmap");
        eng->dirty_bitmap_len = num_tiles;
    }

    WGPUTextureDescriptor tex_desc = {0};
    tex_desc.size = (WGPUExtent3D){width, height, 1};
    tex_desc.format = WGPUTextureFormat_BGRA8Unorm;
    tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;
    tex_desc.dimension = WGPUTextureDimension_2D;
    eng->prev_texture = wgpuDeviceCreateTexture(eng->device, &tex_desc);
    if (!eng->prev_texture)
        return YETTY_ERR(yetty_ycore_void, "failed to create prev texture");

    WGPUBufferDescriptor buf_desc = {0};
    buf_desc.size = num_tiles * sizeof(uint32_t);
    buf_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc |
                     WGPUBufferUsage_CopyDst;
    eng->dirty_flags_buffer = wgpuDeviceCreateBuffer(eng->device, &buf_desc);
    if (!eng->dirty_flags_buffer)
        return YETTY_ERR(yetty_ycore_void, "failed to create dirty flags buffer");

    buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    eng->dirty_flags_readback = wgpuDeviceCreateBuffer(eng->device, &buf_desc);
    if (!eng->dirty_flags_readback)
        return YETTY_ERR(yetty_ycore_void, "failed to create flags readback buffer");

    if (!eng->pipeline) {
        struct yetty_ycore_void_result res = create_pipeline(eng);
        if (!YETTY_IS_OK(res))
            return res;
    }

    ydebug("tile_diff: resources for %ux%u, %u tiles", width, height, num_tiles);
    return YETTY_OK_VOID();
}

/*
 * The coroutine body. Runs the GPU work synchronously up to the readback
 * awaits, yields through yplatform_wgpu_buffer_map_await, then calls the
 * sink and unmaps.
 */
static void submit_coro_entry(void *arg)
{
    struct submit_args *args = arg;
    struct yetty_yrender_utils_tile_diff_engine *eng = args->eng;
    WGPUTexture texture = args->texture;
    uint32_t width = args->width;
    uint32_t height = args->height;
    yetty_yrender_utils_tile_diff_sink_fn sink_fn = args->sink_fn;
    void *sink_ctx = args->sink_ctx;
    free(args);

    struct yetty_ycore_void_result res = ensure_resources(eng, width, height);
    if (!YETTY_IS_OK(res)) {
        ywarn("tile_diff: ensure_resources failed: %s", res.error.msg);
        return;
    }

    uint32_t num_tiles = eng->tiles_x * eng->tiles_y;

    /* Row alignment to WebGPU's 256-byte requirement. */
    uint32_t aligned_bytes_per_row = (width * 4 + 255) & ~255u;
    uint32_t full_buf_size = aligned_bytes_per_row * height;

    if (!eng->tile_readback_buffer ||
        eng->tile_readback_buffer_size != full_buf_size) {
        if (eng->tile_readback_buffer)
            wgpuBufferRelease(eng->tile_readback_buffer);
        WGPUBufferDescriptor buf_desc = {0};
        buf_desc.size = full_buf_size;
        buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        eng->tile_readback_buffer = wgpuDeviceCreateBuffer(eng->device, &buf_desc);
        eng->tile_readback_buffer_size = full_buf_size;
    }

    bool do_full = !eng->prev_has_content || eng->force_full_frame ||
                   eng->always_full_frame;
    if (do_full) {
        eng->force_full_frame = false;
        memset(eng->dirty_bitmap, 1, num_tiles);
    }

    WGPUCommandEncoder encoder =
        wgpuDeviceCreateCommandEncoder(eng->device, NULL);

    /* Only run the diff compute pass when we're doing an incremental frame. */
    WGPUBindGroup diff_bind_group = NULL;
    if (!do_full) {
        WGPUTextureViewDescriptor view_desc = {0};
        view_desc.format = WGPUTextureFormat_BGRA8Unorm;
        view_desc.dimension = WGPUTextureViewDimension_2D;
        view_desc.mipLevelCount = 1;
        view_desc.arrayLayerCount = 1;
        WGPUTextureView curr_view = wgpuTextureCreateView(texture, &view_desc);
        WGPUTextureView prev_view = wgpuTextureCreateView(eng->prev_texture, &view_desc);

        WGPUBindGroupEntry entries[3] = {0};
        entries[0].binding = 0;
        entries[0].textureView = curr_view;
        entries[1].binding = 1;
        entries[1].textureView = prev_view;
        entries[2].binding = 2;
        entries[2].buffer = eng->dirty_flags_buffer;
        entries[2].size = num_tiles * sizeof(uint32_t);

        WGPUBindGroupDescriptor bg_desc = {0};
        bg_desc.layout = eng->bind_group_layout;
        bg_desc.entryCount = 3;
        bg_desc.entries = entries;
        diff_bind_group = wgpuDeviceCreateBindGroup(eng->device, &bg_desc);

        wgpuTextureViewRelease(curr_view);
        wgpuTextureViewRelease(prev_view);

        wgpuCommandEncoderClearBuffer(encoder, eng->dirty_flags_buffer,
                                      0, num_tiles * sizeof(uint32_t));
        WGPUComputePassDescriptor cp_desc = {0};
        WGPUComputePassEncoder cpass =
            wgpuCommandEncoderBeginComputePass(encoder, &cp_desc);
        wgpuComputePassEncoderSetPipeline(cpass, eng->pipeline);
        wgpuComputePassEncoderSetBindGroup(cpass, 0, diff_bind_group, 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(cpass, eng->tiles_x,
                                                 eng->tiles_y, 1);
        wgpuComputePassEncoderEnd(cpass);
        wgpuComputePassEncoderRelease(cpass);

        wgpuCommandEncoderCopyBufferToBuffer(encoder, eng->dirty_flags_buffer,
                                             0, eng->dirty_flags_readback,
                                             0, num_tiles * sizeof(uint32_t));
    }

    /* Snapshot this frame as next frame's baseline. */
    {
        WGPUTexelCopyTextureInfo src = {0};
        src.texture = texture;
        WGPUTexelCopyTextureInfo dst = {0};
        dst.texture = eng->prev_texture;
        WGPUExtent3D extent = {width, height, 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &src, &dst, &extent);
    }

    /* Copy the full current texture to the readback buffer. */
    {
        WGPUTexelCopyTextureInfo src = {0};
        src.texture = texture;
        WGPUTexelCopyBufferInfo dst = {0};
        dst.buffer = eng->tile_readback_buffer;
        dst.layout.bytesPerRow = aligned_bytes_per_row;
        dst.layout.rowsPerImage = height;
        WGPUExtent3D size = {width, height, 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &size);
    }

    WGPUCommandBuffer cmd_buf = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(eng->queue, 1, &cmd_buf);
    wgpuCommandBufferRelease(cmd_buf);
    wgpuCommandEncoderRelease(encoder);

    if (diff_bind_group)
        wgpuBindGroupRelease(diff_bind_group);

    /* Texture is no longer touched past this point — caller may release it. */

    if (!do_full) {
        res = yplatform_wgpu_buffer_map_await(eng->wgpu,
            eng->dirty_flags_readback, WGPUMapMode_Read,
            0, num_tiles * sizeof(uint32_t));
        if (!YETTY_IS_OK(res)) {
            ywarn("tile_diff: flags map failed: %s", res.error.msg);
            return;
        }
        const uint32_t *flags = wgpuBufferGetConstMappedRange(
            eng->dirty_flags_readback,
            0, num_tiles * sizeof(uint32_t));
        for (uint32_t i = 0; i < num_tiles; i++)
            eng->dirty_bitmap[i] = flags[i] ? 1u : 0u;
        wgpuBufferUnmap(eng->dirty_flags_readback);
    }

    res = yplatform_wgpu_buffer_map_await(eng->wgpu,
        eng->tile_readback_buffer, WGPUMapMode_Read, 0, full_buf_size);
    if (!YETTY_IS_OK(res)) {
        ywarn("tile_diff: pixels map failed: %s", res.error.msg);
        return;
    }

    eng->prev_has_content = true;

    uint32_t dirty_count = 0;
    for (uint32_t i = 0; i < num_tiles; i++)
        if (eng->dirty_bitmap[i])
            dirty_count++;

    const uint8_t *mapped = wgpuBufferGetConstMappedRange(
        eng->tile_readback_buffer, 0, full_buf_size);

    struct yetty_yrender_utils_tile_diff_frame frame = {
        .pixels = mapped,
        .aligned_bytes_per_row = aligned_bytes_per_row,
        .width = width,
        .height = height,
        .tile_size = eng->tile_size,
        .tiles_x = eng->tiles_x,
        .tiles_y = eng->tiles_y,
        .dirty_bitmap = eng->dirty_bitmap,
        .dirty_count = dirty_count,
    };

    if (sink_fn)
        sink_fn(sink_ctx, &frame);

    wgpuBufferUnmap(eng->tile_readback_buffer);
}

struct yetty_yrender_utils_tile_diff_engine_ptr_result
yetty_yrender_utils_tile_diff_engine_create(WGPUDevice device,
                                            WGPUQueue queue,
                                            struct yplatform_wgpu *wgpu,
                                            uint32_t tile_size)
{
    if (!device || !queue || !wgpu || tile_size == 0)
        return YETTY_ERR(yetty_yrender_utils_tile_diff_engine_ptr,
                         "invalid arguments");

    struct yetty_yrender_utils_tile_diff_engine *eng = calloc(1, sizeof(*eng));
    if (!eng)
        return YETTY_ERR(yetty_yrender_utils_tile_diff_engine_ptr,
                         "calloc failed");

    eng->device = device;
    eng->queue = queue;
    eng->wgpu = wgpu;
    eng->tile_size = tile_size;

    return YETTY_OK(yetty_yrender_utils_tile_diff_engine_ptr, eng);
}

void yetty_yrender_utils_tile_diff_engine_destroy(
    struct yetty_yrender_utils_tile_diff_engine *eng)
{
    if (!eng)
        return;

    if (eng->pipeline)
        wgpuComputePipelineRelease(eng->pipeline);
    if (eng->bind_group_layout)
        wgpuBindGroupLayoutRelease(eng->bind_group_layout);
    if (eng->prev_texture)
        wgpuTextureRelease(eng->prev_texture);
    if (eng->dirty_flags_buffer)
        wgpuBufferRelease(eng->dirty_flags_buffer);
    if (eng->dirty_flags_readback)
        wgpuBufferRelease(eng->dirty_flags_readback);
    if (eng->tile_readback_buffer)
        wgpuBufferRelease(eng->tile_readback_buffer);

    free(eng->dirty_bitmap);
    free(eng);
}

void yetty_yrender_utils_tile_diff_engine_force_full(
    struct yetty_yrender_utils_tile_diff_engine *eng)
{
    if (eng)
        eng->force_full_frame = true;
}

void yetty_yrender_utils_tile_diff_engine_set_always_full(
    struct yetty_yrender_utils_tile_diff_engine *eng, bool on)
{
    if (eng)
        eng->always_full_frame = on;
}

struct yetty_ycore_void_result
yetty_yrender_utils_tile_diff_engine_submit(
    struct yetty_yrender_utils_tile_diff_engine *eng,
    WGPUTexture texture, uint32_t width, uint32_t height,
    yetty_yrender_utils_tile_diff_sink_fn sink_fn, void *sink_ctx)
{
    if (!eng || !texture || width == 0 || height == 0)
        return YETTY_ERR(yetty_ycore_void, "invalid arguments");

    struct submit_args *args = malloc(sizeof(*args));
    if (!args)
        return YETTY_ERR(yetty_ycore_void, "malloc failed");

    args->eng = eng;
    args->texture = texture;
    args->width = width;
    args->height = height;
    args->sink_fn = sink_fn;
    args->sink_ctx = sink_ctx;

    struct yplatform_coro_ptr_result coro_res = yplatform_coro_spawn(
        submit_coro_entry, args, 0, "tile-diff-submit");
    if (!YETTY_IS_OK(coro_res)) {
        free(args);
        return YETTY_ERR(yetty_ycore_void, "tile_diff coro spawn failed");
    }

    yplatform_coro_resume(coro_res.value);
    if (yplatform_coro_is_finished(coro_res.value))
        yplatform_coro_destroy(coro_res.value);
    /* Otherwise the coro yielded into the GPU await; resume_coro_on_loop
     * (in ywebgpu.c) destroys it once it finishes. Mirrors the old VNC path. */

    return YETTY_OK_VOID();
}
