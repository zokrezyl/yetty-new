/*
 * ymgui-layer.c — cursor-anchored, scroll-synced Dear ImGui layer.
 *
 * GPU model
 *   The layer owns its own WGPURenderPipeline (compiled ONCE at first
 *   render, then cached for the layer's lifetime), its own vertex/index
 *   WGPUBuffers (grown on demand), its own atlas texture + sampler, and
 *   its own bind group. It bypasses the gpu-resource-binder fullscreen-
 *   quad path entirely — that abstraction is for SDF-style "one fragment
 *   shader over the whole viewport" layers, and ImGui needs the opposite
 *   shape: many indexed-triangle draw calls with per-call scissor.
 *
 * Frame
 *   Each --frame OSC carries an ImDrawData mesh in our wire format
 *   (include/yetty/ymgui/wire.h). On render(), the layer:
 *     1. compiles its pipeline if not yet cached
 *     2. uploads vtx/idx straight to GPU (grow buffers if needed)
 *     3. begins a render pass on the target's view (LoadOp_Clear)
 *     4. iterates ImDrawCmds — setScissorRect + drawIndexed per call
 *     5. ends pass + submits
 *
 * Scrolling
 *   Anchored at the cursor's rolling row at frame arrival (ypaint-canvas
 *   model). Scroll is O(1): the vertex shader adds a y-offset uniform,
 *   computed each frame from (frame_rolling_row - row_origin)*cell_h.
 *   No re-upload of geometry on scroll.
 *
 * Atlas
 *   ImGui's font atlas comes once via --tex (R8). We create a dedicated
 *   R8Unorm WGPUTexture; the fragment shader reads .r as alpha and
 *   multiplies by vertex color — that's the standard ImGui shader for
 *   alpha8 atlases. Backgrounds use the atlas's "white pixel" so they
 *   sample 1.0 and end up with vertex color unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webgpu/webgpu.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/util.h>
#include <yetty/yconfig.h>
#include <yetty/yetty.h>
#include <yetty/yface/yface.h>
#include <yetty/ymgui/wire.h>
#include <yetty/yrender/render-target.h>
#include <yetty/yterm/osc-args.h>
#include <yetty/yterm/pty-reader.h>
#include <yetty/yterm/terminal.h>
#include <yetty/yterm/ymgui-layer.h>
#include <yetty/ytrace.h>

/*===========================================================================
 * Layer object
 *=========================================================================*/

struct yetty_yterm_ymgui_layer {
    struct yetty_yterm_terminal_layer base;

    /* GPU context — cached at create. */
    WGPUDevice         device;
    WGPUQueue          queue;
    WGPUTextureFormat  target_format;

    /* WGSL source loaded from paths/shaders/ymgui-layer.wgsl at create
     * time. Heap-owned; kept alive until destroy because the WebGPU
     * shader-module create call may reference it during async compile.
     * Never re-read from disk — pipeline is compiled once and cached. */
    struct yetty_ycore_buffer shader_code;

    /* Pipeline (compiled once, cached). */
    int                pipeline_ready;
    WGPUShaderModule   shader_module;
    WGPUBindGroupLayout bind_group_layout;
    WGPUPipelineLayout pipeline_layout;
    WGPURenderPipeline pipeline;
    WGPUSampler        sampler;
    WGPUBuffer         uniform_buffer;       /* fixed 32 B */

    /* Atlas (rebuilt each --tex). */
    int                atlas_ready;
    uint32_t           atlas_w;
    uint32_t           atlas_h;
    WGPUTexture        atlas_texture;
    WGPUTextureView    atlas_view;

    /* Bind group — rebuilt when atlas changes. */
    WGPUBindGroup      bind_group;

    /* Geometry buffers — grown with 25% headroom on demand. */
    WGPUBuffer         vtx_buf;
    size_t             vtx_buf_capacity;     /* in bytes */
    WGPUBuffer         idx_buf;
    size_t             idx_buf_capacity;     /* in bytes */

    /* Latest decoded frame (owning copy of the OSC payload). */
    uint8_t           *frame_bytes;
    size_t             frame_size;
    int                has_frame;

    /* Streaming OSC decoder — feeds base64 → LZ4F → in_buf. Reused
     * across emits, so we don't re-malloc the LZ4 dictionary each call. */
    struct yetty_yface *yface;

    /* Scrolling anchor (same model as yetty_ypaint_canvas). */
    uint32_t           frame_rolling_row;
    uint32_t           row0_absolute;
    uint32_t           cursor_row;
    uint32_t           cursor_col;
    float              frame_display_w;
    float              frame_display_h;
};

/*===========================================================================
 * Forward declarations
 *=========================================================================*/

static void ymgui_destroy(struct yetty_yterm_terminal_layer *self);
static struct yetty_ycore_void_result
ymgui_write(struct yetty_yterm_terminal_layer *self,
            int osc_code, const char *data, size_t len);
static struct yetty_ycore_void_result
ymgui_resize_grid(struct yetty_yterm_terminal_layer *self, struct grid_size gs);
static struct yetty_ycore_void_result
ymgui_set_cell_size(struct yetty_yterm_terminal_layer *self, struct pixel_size cs);
static struct yetty_ycore_void_result
ymgui_set_visual_zoom(struct yetty_yterm_terminal_layer *self,
                      float scale, float off_x, float off_y);
static struct yetty_yrender_gpu_resource_set_result
ymgui_get_gpu_resource_set(const struct yetty_yterm_terminal_layer *self);
static struct yetty_ycore_void_result
ymgui_render(struct yetty_yterm_terminal_layer *self,
             struct yetty_yrender_target *target);
static int  ymgui_is_empty(const struct yetty_yterm_terminal_layer *self);
static int  ymgui_on_key (struct yetty_yterm_terminal_layer *self, int key, int mods);
static int  ymgui_on_char(struct yetty_yterm_terminal_layer *self,
                          uint32_t cp, int mods);
static struct yetty_ycore_void_result
ymgui_scroll(struct yetty_yterm_terminal_layer *self, int lines);
static void ymgui_set_cursor(struct yetty_yterm_terminal_layer *self,
                             int col, int row);
static void ymgui_get_input_origin(const struct yetty_yterm_terminal_layer *self,
                                   float *out_x, float *out_y);

static const struct yetty_yterm_terminal_layer_ops ymgui_ops = {
    .destroy              = ymgui_destroy,
    .write                = ymgui_write,
    .resize_grid          = ymgui_resize_grid,
    .set_cell_size        = ymgui_set_cell_size,
    .set_visual_zoom      = ymgui_set_visual_zoom,
    .get_gpu_resource_set = ymgui_get_gpu_resource_set,
    .render               = ymgui_render,
    .is_empty             = ymgui_is_empty,
    .on_key               = ymgui_on_key,
    .on_char              = ymgui_on_char,
    .scroll               = ymgui_scroll,
    .set_cursor           = ymgui_set_cursor,
    .get_input_origin     = ymgui_get_input_origin,
};

/*===========================================================================
 * Pipeline + bind group (cached — built once)
 *=========================================================================*/

static void release_pipeline(struct yetty_yterm_ymgui_layer *l)
{
    if (l->pipeline)          { wgpuRenderPipelineRelease(l->pipeline);     l->pipeline = NULL; }
    if (l->pipeline_layout)   { wgpuPipelineLayoutRelease(l->pipeline_layout); l->pipeline_layout = NULL; }
    if (l->bind_group_layout) { wgpuBindGroupLayoutRelease(l->bind_group_layout); l->bind_group_layout = NULL; }
    if (l->shader_module)     { wgpuShaderModuleRelease(l->shader_module);  l->shader_module = NULL; }
    if (l->sampler)           { wgpuSamplerRelease(l->sampler);             l->sampler = NULL; }
    if (l->uniform_buffer)    { wgpuBufferRelease(l->uniform_buffer);       l->uniform_buffer = NULL; }
    l->pipeline_ready = 0;
}

static int build_pipeline(struct yetty_yterm_ymgui_layer *l)
{
    /* Shader module — built once from the WGSL we loaded at create time. */
    WGPUShaderSourceWGSL wgsl = {0};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = (WGPUStringView){ (const char *)l->shader_code.data,
                                  l->shader_code.size };

    WGPUShaderModuleDescriptor sm_desc = {0};
    sm_desc.nextInChain = &wgsl.chain;
    l->shader_module = wgpuDeviceCreateShaderModule(l->device, &sm_desc);
    if (!l->shader_module) {
        yerror("ymgui: shader module creation failed");
        return 0;
    }

    /* Bind group layout: uniform + texture + sampler. */
    WGPUBindGroupLayoutEntry bgl_entries[3] = {0};
    bgl_entries[0].binding = 0;
    bgl_entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bgl_entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bgl_entries[0].buffer.minBindingSize = 32;

    bgl_entries[1].binding = 1;
    bgl_entries[1].visibility = WGPUShaderStage_Fragment;
    bgl_entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    bgl_entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

    bgl_entries[2].binding = 2;
    bgl_entries[2].visibility = WGPUShaderStage_Fragment;
    bgl_entries[2].sampler.type = WGPUSamplerBindingType_Filtering;

    WGPUBindGroupLayoutDescriptor bgl_desc = {0};
    bgl_desc.entryCount = 3;
    bgl_desc.entries    = bgl_entries;
    l->bind_group_layout = wgpuDeviceCreateBindGroupLayout(l->device, &bgl_desc);
    if (!l->bind_group_layout) return 0;

    /* Pipeline layout */
    WGPUPipelineLayoutDescriptor pl_desc = {0};
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts     = &l->bind_group_layout;
    l->pipeline_layout = wgpuDeviceCreatePipelineLayout(l->device, &pl_desc);
    if (!l->pipeline_layout) return 0;

    /* Vertex layout — matches ImDrawVert exactly (20 bytes). */
    WGPUVertexAttribute vattrs[3] = {0};
    vattrs[0].format = WGPUVertexFormat_Float32x2;     /* pos */
    vattrs[0].offset = 0;
    vattrs[0].shaderLocation = 0;
    vattrs[1].format = WGPUVertexFormat_Float32x2;     /* uv  */
    vattrs[1].offset = 8;
    vattrs[1].shaderLocation = 1;
    vattrs[2].format = WGPUVertexFormat_Unorm8x4;      /* col */
    vattrs[2].offset = 16;
    vattrs[2].shaderLocation = 2;

    WGPUVertexBufferLayout vbl = {0};
    vbl.stepMode      = WGPUVertexStepMode_Vertex;
    vbl.arrayStride   = 20;
    vbl.attributeCount = 3;
    vbl.attributes    = vattrs;

    /* Color target — standard ImGui alpha blend. */
    WGPUBlendComponent blend_color = {
        .operation = WGPUBlendOperation_Add,
        .srcFactor = WGPUBlendFactor_SrcAlpha,
        .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
    };
    WGPUBlendComponent blend_alpha = {
        .operation = WGPUBlendOperation_Add,
        .srcFactor = WGPUBlendFactor_One,
        .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
    };
    WGPUBlendState blend = { .color = blend_color, .alpha = blend_alpha };
    WGPUColorTargetState color_target = {0};
    color_target.format    = l->target_format;
    color_target.blend     = &blend;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fs = {0};
    fs.module      = l->shader_module;
    fs.entryPoint  = (WGPUStringView){ "fs_main", 7 };
    fs.targetCount = 1;
    fs.targets     = &color_target;

    WGPURenderPipelineDescriptor rpd = {0};
    rpd.layout                = l->pipeline_layout;
    rpd.vertex.module         = l->shader_module;
    rpd.vertex.entryPoint     = (WGPUStringView){ "vs_main", 7 };
    rpd.vertex.bufferCount    = 1;
    rpd.vertex.buffers        = &vbl;
    rpd.primitive.topology    = WGPUPrimitiveTopology_TriangleList;
    rpd.primitive.frontFace   = WGPUFrontFace_CCW;
    rpd.primitive.cullMode    = WGPUCullMode_None;
    rpd.fragment              = &fs;
    rpd.multisample.count     = 1;
    rpd.multisample.mask      = 0xFFFFFFFFu;

    l->pipeline = wgpuDeviceCreateRenderPipeline(l->device, &rpd);
    if (!l->pipeline) {
        yerror("ymgui: render pipeline creation failed");
        return 0;
    }

    /* Sampler — linear, clamp. */
    WGPUSamplerDescriptor sd = {0};
    sd.addressModeU = WGPUAddressMode_ClampToEdge;
    sd.addressModeV = WGPUAddressMode_ClampToEdge;
    sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter    = WGPUFilterMode_Linear;
    sd.minFilter    = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.maxAnisotropy = 1;
    l->sampler = wgpuDeviceCreateSampler(l->device, &sd);

    /* Uniform buffer — 32 B (16 used + 16 pad for std140 vec2 alignment). */
    WGPUBufferDescriptor ub = {0};
    ub.size  = 32;
    ub.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    l->uniform_buffer = wgpuDeviceCreateBuffer(l->device, &ub);

    l->pipeline_ready = 1;
    ydebug("ymgui: pipeline compiled and cached");
    return 1;
}

static void rebuild_bind_group(struct yetty_yterm_ymgui_layer *l)
{
    if (l->bind_group) { wgpuBindGroupRelease(l->bind_group); l->bind_group = NULL; }
    if (!l->pipeline_ready || !l->atlas_ready) return;

    WGPUBindGroupEntry e[3] = {0};
    e[0].binding = 0;
    e[0].buffer  = l->uniform_buffer;
    e[0].size    = 32;
    e[1].binding = 1;
    e[1].textureView = l->atlas_view;
    e[2].binding = 2;
    e[2].sampler = l->sampler;

    WGPUBindGroupDescriptor bgd = {0};
    bgd.layout     = l->bind_group_layout;
    bgd.entryCount = 3;
    bgd.entries    = e;
    l->bind_group  = wgpuDeviceCreateBindGroup(l->device, &bgd);
}

/*===========================================================================
 * Vertex / index buffer growth
 *=========================================================================*/

static int ensure_buffer(WGPUDevice dev, WGPUBuffer *buf, size_t *cap,
                         size_t need, WGPUBufferUsage usage)
{
    if (need <= *cap && *buf) return 1;
    if (*buf) { wgpuBufferRelease(*buf); *buf = NULL; }
    /* 25 % headroom + minimum 4 KB so re-uploads don't churn. */
    size_t new_cap = need + need / 4u;
    if (new_cap < 4096) new_cap = 4096;
    /* WebGPU buffer copy size must be a multiple of 4. */
    new_cap = (new_cap + 3u) & ~(size_t)3u;
    WGPUBufferDescriptor bd = {0};
    bd.size  = new_cap;
    bd.usage = usage;
    *buf = wgpuDeviceCreateBuffer(dev, &bd);
    if (!*buf) return 0;
    *cap = new_cap;
    return 1;
}

/*===========================================================================
 * Atlas upload (--tex)
 *=========================================================================*/

static void release_atlas(struct yetty_yterm_ymgui_layer *l)
{
    if (l->bind_group)    { wgpuBindGroupRelease(l->bind_group);     l->bind_group = NULL; }
    if (l->atlas_view)    { wgpuTextureViewRelease(l->atlas_view);   l->atlas_view = NULL; }
    if (l->atlas_texture) { wgpuTextureDestroy(l->atlas_texture);
                            wgpuTextureRelease(l->atlas_texture);    l->atlas_texture = NULL; }
    l->atlas_ready = 0;
    l->atlas_w = l->atlas_h = 0;
}

static int upload_atlas(struct yetty_yterm_ymgui_layer *l,
                        const struct ymgui_wire_tex *th)
{
    /* Only R8 is supported on the wire today (frontend ships Alpha8). The
     * standard ImGui shader treats .r as alpha; we follow that. RGBA8
     * support is a follow-up if we ever expose user textures. */
    if (th->format != YMGUI_TEX_FMT_R8) {
        yerror("ymgui: --tex format %u not supported (R8 only)", th->format);
        return 0;
    }

    release_atlas(l);

    WGPUTextureDescriptor td = {0};
    td.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension     = WGPUTextureDimension_2D;
    td.size.width    = th->width;
    td.size.height   = th->height;
    td.size.depthOrArrayLayers = 1;
    td.format        = WGPUTextureFormat_R8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount   = 1;
    l->atlas_texture = wgpuDeviceCreateTexture(l->device, &td);
    if (!l->atlas_texture) return 0;

    WGPUTextureViewDescriptor vd = {0};
    vd.format          = WGPUTextureFormat_R8Unorm;
    vd.dimension       = WGPUTextureViewDimension_2D;
    vd.mipLevelCount   = 1;
    vd.arrayLayerCount = 1;
    vd.aspect          = WGPUTextureAspect_All;
    l->atlas_view = wgpuTextureCreateView(l->atlas_texture, &vd);

    /* Pixel data follows the wire header. */
    const uint8_t *pixels = (const uint8_t *)(th + 1);
    size_t pixel_bytes = (size_t)th->width * (size_t)th->height; /* R8 */

    WGPUTexelCopyTextureInfo dest = {0};
    dest.texture = l->atlas_texture;
    WGPUTexelCopyBufferLayout src_layout = {0};
    src_layout.bytesPerRow  = th->width;
    src_layout.rowsPerImage = th->height;
    WGPUExtent3D extent = { th->width, th->height, 1 };
    wgpuQueueWriteTexture(l->queue, &dest, pixels, pixel_bytes,
                          &src_layout, &extent);

    l->atlas_w = th->width;
    l->atlas_h = th->height;
    l->atlas_ready = 1;

    /* Bind group references atlas_view, so it must be rebuilt whenever the
     * atlas is replaced — bind groups in WebGPU are immutable references. */
    rebuild_bind_group(l);

    ydebug("ymgui: atlas uploaded %ux%u R8", th->width, th->height);
    return 1;
}

/*===========================================================================
 * Wire decoding
 * (b64 + LZ4F decompression now handled by yetty_yface; this section is
 *  just struct validators for the post-decompression payload bytes.)
 *=========================================================================*/

static int validate_frame(const uint8_t *data, size_t size,
                          const struct ymgui_wire_frame **out_hdr)
{
    if (size < sizeof(struct ymgui_wire_frame)) return -1;
    const struct ymgui_wire_frame *fh = (const struct ymgui_wire_frame *)data;
    if (fh->magic   != YMGUI_WIRE_MAGIC_FRAME) return -1;
    if (fh->version != YMGUI_WIRE_VERSION)     return -1;
    if (fh->total_size != size)                return -1;
    *out_hdr = fh;
    return 0;
}

static int validate_tex(const uint8_t *data, size_t size,
                        const struct ymgui_wire_tex **out_hdr)
{
    if (size < sizeof(struct ymgui_wire_tex)) return -1;
    const struct ymgui_wire_tex *th = (const struct ymgui_wire_tex *)data;
    if (th->magic   != YMGUI_WIRE_MAGIC_TEX) return -1;
    if (th->version != YMGUI_WIRE_VERSION)   return -1;
    if (th->total_size != size)              return -1;
    uint32_t bpp = (th->format == YMGUI_TEX_FMT_R8) ? 1u
                 : (th->format == YMGUI_TEX_FMT_RGBA8) ? 4u : 0u;
    if (bpp == 0) return -1;
    if ((size_t)th->total_size != sizeof(*th) +
        (size_t)th->width * (size_t)th->height * bpp) return -1;
    *out_hdr = th;
    return 0;
}

/*===========================================================================
 * Frame anchor / fit-under-cursor
 *=========================================================================*/

static uint32_t frame_row_span(const struct yetty_yterm_ymgui_layer *l)
{
    if (!l->has_frame || l->base.cell_size.height <= 0.0f) return 0;
    uint32_t rows = (uint32_t)((l->frame_display_h + l->base.cell_size.height
                                - 1.0f) / l->base.cell_size.height);
    return rows ? rows : 1u;
}

static void anchor_frame_and_fit(struct yetty_yterm_ymgui_layer *l)
{
    l->frame_rolling_row = l->row0_absolute + l->cursor_row;

    uint32_t span = frame_row_span(l);
    uint32_t rows = l->base.grid_size.rows;
    uint32_t room_below = (l->cursor_row >= rows) ? 0u
                                                  : (rows - l->cursor_row);

    if (span > room_below && l->base.scroll_fn && !l->base.in_external_scroll) {
        int need = (int)(span - room_below);
        struct yetty_ycore_void_result r = l->base.scroll_fn(
            &l->base, need, l->base.scroll_userdata);
        if (YETTY_IS_ERR(r)) yerror("ymgui: scroll_fn failed: %s", r.error.msg);
    }
}

/*===========================================================================
 * Create / destroy
 *=========================================================================*/

struct yetty_yterm_terminal_layer_result yetty_yterm_ymgui_layer_create(
    uint32_t cols, uint32_t rows, float cell_w, float cell_h,
    const struct yetty_context *context,
    yetty_yterm_request_render_fn request_render_fn,
    void *request_render_userdata,
    yetty_yterm_scroll_fn scroll_fn, void *scroll_userdata,
    yetty_yterm_cursor_fn cursor_fn, void *cursor_userdata)
{
    if (!context)
        return YETTY_ERR(yetty_yterm_terminal_layer, "context is NULL");
    if (!context->gpu_context.device || !context->gpu_context.queue)
        return YETTY_ERR(yetty_yterm_terminal_layer, "gpu context is incomplete");

    /* Load the WGSL source from paths/shaders — same pattern as ypaint /
     * text layers. Loaded ONCE at create time; the pipeline compiled from
     * it is cached for the life of the layer. */
    struct yetty_yconfig *cfg = context->app_context.config;
    const char *shaders_dir = cfg->ops->get_string(cfg, "paths/shaders", "");
    char shader_path[512];
    snprintf(shader_path, sizeof(shader_path),
             "%s/ymgui-layer.wgsl", shaders_dir);
    struct yetty_ycore_buffer_result shader_res =
        yetty_ycore_read_file(shader_path);
    if (YETTY_IS_ERR(shader_res))
        return YETTY_ERR(yetty_yterm_terminal_layer, shader_res.error.msg);

    struct yetty_yterm_ymgui_layer *l = calloc(1, sizeof(*l));
    if (!l) {
        free(shader_res.value.data);
        return YETTY_ERR(yetty_yterm_terminal_layer, "alloc failed");
    }
    l->shader_code = shader_res.value;

    l->base.ops                       = &ymgui_ops;
    l->base.grid_size.cols            = cols;
    l->base.grid_size.rows            = rows;
    l->base.cell_size.width           = cell_w;
    l->base.cell_size.height          = cell_h;
    l->base.dirty                     = 0;
    l->base.request_render_fn         = request_render_fn;
    l->base.request_render_userdata   = request_render_userdata;
    l->base.scroll_fn                 = scroll_fn;
    l->base.scroll_userdata           = scroll_userdata;
    l->base.cursor_fn                 = cursor_fn;
    l->base.cursor_userdata           = cursor_userdata;

    l->device        = context->gpu_context.device;
    l->queue         = context->gpu_context.queue;
    l->target_format = context->gpu_context.surface_format;

    /* yface holds the streaming b64 decoder + LZ4F decompression context.
     * Reused across all incoming --frame / --tex emits — never destroyed
     * until the layer goes away. */
    {
        struct yetty_yface_ptr_result yr = yetty_yface_create();
        if (YETTY_IS_ERR(yr)) {
            free(l->shader_code.data);
            free(l);
            return YETTY_ERR(yetty_yterm_terminal_layer, yr.error.msg);
        }
        l->yface = yr.value;
    }

    ydebug("ymgui_layer_create: %ux%u grid, %.1fx%.1f cell, format=%u",
           cols, rows, cell_w, cell_h, (unsigned)l->target_format);

    return YETTY_OK(yetty_yterm_terminal_layer, &l->base);
}

static void ymgui_destroy(struct yetty_yterm_terminal_layer *self)
{
    struct yetty_yterm_ymgui_layer *l = (struct yetty_yterm_ymgui_layer *)self;
    if (!l) return;
    release_atlas(l);
    release_pipeline(l);
    if (l->vtx_buf) wgpuBufferRelease(l->vtx_buf);
    if (l->idx_buf) wgpuBufferRelease(l->idx_buf);
    if (l->yface) yetty_yface_destroy(l->yface);
    free(l->shader_code.data);
    free(l->frame_bytes);
    free(l);
}

/*===========================================================================
 * write — OSC dispatch (--frame / --tex / --clear)
 *=========================================================================*/

static struct yetty_ycore_void_result
handle_frame(struct yetty_yterm_ymgui_layer *l,
             const uint8_t *raw, size_t raw_size)
{
    const struct ymgui_wire_frame *fh = NULL;
    if (validate_frame(raw, raw_size, &fh) != 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: malformed --frame payload");

    uint8_t *copy = (uint8_t *)malloc(raw_size);
    if (!copy) return YETTY_ERR(yetty_ycore_void, "ymgui: oom");
    memcpy(copy, raw, raw_size);

    free(l->frame_bytes);
    l->frame_bytes     = copy;
    l->frame_size      = raw_size;
    l->has_frame       = 1;
    l->frame_display_w = fh->display_size_x;
    l->frame_display_h = fh->display_size_y;

    anchor_frame_and_fit(l);

    l->base.dirty = 1;
    if (l->base.request_render_fn)
        l->base.request_render_fn(l->base.request_render_userdata);
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
handle_tex(struct yetty_yterm_ymgui_layer *l,
           const uint8_t *raw, size_t raw_size)
{
    const struct ymgui_wire_tex *th = NULL;
    if (validate_tex(raw, raw_size, &th) != 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: malformed --tex payload");
    if (th->tex_id != YMGUI_TEX_ID_FONT_ATLAS)
        return YETTY_OK_VOID();   /* user textures: future work */
    if (!upload_atlas(l, th))
        return YETTY_ERR(yetty_ycore_void, "ymgui: atlas upload failed");
    return YETTY_OK_VOID();
}

/* Body shape after pty-reader strips "<code>;" is "<b64-args>;<b64-payload>".
 * For frame/tex the args slot carries a yetty_yface_bin_meta; we just
 * skip past the first ';' and feed the payload to yface (compressed
 * is implicit per code: frame and tex are always LZ4F-compressed). */
static struct yetty_ycore_void_result
ymgui_decode(struct yetty_yterm_ymgui_layer *l,
             const char *data, size_t len)
{
    /* Find the args/payload separator. */
    const char *payload = NULL;
    size_t      payload_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == ';') {
            payload     = data + i + 1;
            payload_len = len - i - 1;
            break;
        }
    }
    if (!payload)
        return YETTY_ERR(yetty_ycore_void, "ymgui: malformed body (need ;)");

    struct yetty_ycore_void_result r =
        yetty_yface_start_read(l->yface, /*compressed=*/1);
    if (YETTY_IS_ERR(r)) return r;
    r = yetty_yface_feed(l->yface, payload, payload_len);
    if (YETTY_IS_ERR(r)) { yetty_yface_finish_read(l->yface); return r; }
    return yetty_yface_finish_read(l->yface);
}

static struct yetty_ycore_void_result
ymgui_write(struct yetty_yterm_terminal_layer *self,
            int osc_code, const char *data, size_t len)
{
    struct yetty_yterm_ymgui_layer *l = (struct yetty_yterm_ymgui_layer *)self;

    /* Clear has an empty body — short-circuit before splitting. */
    if (osc_code == YMGUI_OSC_CS_CLEAR) {
        free(l->frame_bytes);
        l->frame_bytes = NULL;
        l->frame_size  = 0;
        l->has_frame   = 0;
        l->base.dirty  = 1;
        if (l->base.request_render_fn)
            l->base.request_render_fn(l->base.request_render_userdata);
        return YETTY_OK_VOID();
    }

    if (!data || len == 0)
        return YETTY_ERR(yetty_ycore_void, "ymgui: empty OSC body");

    struct yetty_ycore_void_result r = ymgui_decode(l, data, len);
    if (YETTY_IS_ERR(r)) return r;

    struct yetty_ycore_buffer *in = yetty_yface_in_buf(l->yface);
    switch (osc_code) {
    case YMGUI_OSC_CS_FRAME: return handle_frame(l, in->data, in->size);
    case YMGUI_OSC_CS_TEX:   return handle_tex  (l, in->data, in->size);
    default:
        return YETTY_ERR(yetty_ycore_void, "ymgui: unexpected OSC code");
    }
}

/*===========================================================================
 * resize / set_cell_size / set_visual_zoom — minimal, no GPU-side reset
 * (the pipeline is independent of grid size).
 *=========================================================================*/

static struct yetty_ycore_void_result
ymgui_resize_grid(struct yetty_yterm_terminal_layer *self, struct grid_size gs)
{
    self->grid_size = gs;
    self->dirty = 1;
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
ymgui_set_cell_size(struct yetty_yterm_terminal_layer *self,
                    struct pixel_size cs)
{
    if (cs.width <= 0.0f || cs.height <= 0.0f)
        return YETTY_ERR(yetty_ycore_void, "ymgui: invalid cell size");
    self->cell_size = cs;
    self->dirty = 1;
    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result
ymgui_set_visual_zoom(struct yetty_yterm_terminal_layer *self,
                      float scale, float off_x, float off_y)
{
    /* Visual zoom is not yet wired into the ymgui pipeline (it would
     * scale the projection matrix in the vertex shader). Accept the call
     * silently so the terminal can still drive the other layers. */
    (void)self; (void)scale; (void)off_x; (void)off_y;
    return YETTY_OK_VOID();
}

/*===========================================================================
 * get_gpu_resource_set — required by the ops table but unused: we
 * never go through render_target->render_layer (that's the binder path).
 * Return a minimal stub pointing at a static empty rs.
 *=========================================================================*/

static struct yetty_yrender_gpu_resource_set_result
ymgui_get_gpu_resource_set(const struct yetty_yterm_terminal_layer *self)
{
    (void)self;
    static const struct yetty_yrender_gpu_resource_set empty = {0};
    return YETTY_OK(yetty_yrender_gpu_resource_set, &empty);
}

/*===========================================================================
 * render — own pipeline, own draw calls, no binder
 *=========================================================================*/

struct draw_pass_inputs {
    /* Pre-walked counts of the wire frame so we can size GPU buffers
     * before encoding. */
    size_t total_vtx_bytes;
    size_t total_idx_bytes;
};

/* Walk the wire frame to size GPU buffers and validate layout. */
static int frame_measure(const struct yetty_yterm_ymgui_layer *l,
                         struct draw_pass_inputs *out, int *out_idx32)
{
    const struct ymgui_wire_frame *fh =
        (const struct ymgui_wire_frame *)l->frame_bytes;
    const uint8_t *cur = l->frame_bytes + sizeof(*fh);
    const uint8_t *end = l->frame_bytes + l->frame_size;
    int idx32 = (fh->flags & YMGUI_FRAME_FLAG_IDX32) ? 1 : 0;
    size_t idx_bpe = idx32 ? 4u : 2u;

    size_t total_vtx = 0, total_idx_bytes = 0;
    for (uint32_t li = 0; li < fh->cmd_list_count; li++) {
        if (cur + sizeof(struct ymgui_wire_cmd_list) > end) return 0;
        const struct ymgui_wire_cmd_list *clh =
            (const struct ymgui_wire_cmd_list *)cur;
        cur += sizeof(*clh);

        size_t vbytes = (size_t)clh->vtx_count * 20u;
        cur += vbytes; if (cur > end) return 0;

        size_t ibytes_padded = (size_t)clh->idx_count * idx_bpe;
        if (ibytes_padded & 3u) ibytes_padded += 4u - (ibytes_padded & 3u);
        cur += ibytes_padded; if (cur > end) return 0;

        cur += (size_t)clh->cmd_count * sizeof(struct ymgui_wire_cmd);
        if (cur > end) return 0;

        total_vtx       += vbytes;
        total_idx_bytes += (size_t)clh->idx_count * 4u;  /* always emit u32 */
    }
    out->total_vtx_bytes = total_vtx;
    out->total_idx_bytes = total_idx_bytes;
    *out_idx32 = idx32;
    return 1;
}

/* Stream verts/idx from the wire to GPU buffers. We always promote indices
 * to u32 on the GPU side — keeps the pipeline single-codepath, costs at
 * most 2x index memory which is small relative to verts. Returns the
 * per-cmd-list base offsets so render() can emit the draw calls. */
struct cl_offsets {
    size_t vtx_byte_offset;   /* in the GPU vertex buffer */
    size_t idx_u32_offset;    /* in the GPU index buffer (u32 elements) */
    uint32_t cmd_count;
    const struct ymgui_wire_cmd *cmds;
    uint32_t vtx_count;       /* for index validation */
};

static int frame_upload(struct yetty_yterm_ymgui_layer *l,
                        struct cl_offsets *cls, size_t cls_max,
                        size_t *cls_count, int idx32)
{
    const struct ymgui_wire_frame *fh =
        (const struct ymgui_wire_frame *)l->frame_bytes;
    const uint8_t *cur = l->frame_bytes + sizeof(*fh);
    const uint8_t *end = l->frame_bytes + l->frame_size;
    size_t idx_bpe = idx32 ? 4u : 2u;

    /* Single CPU-side staging for index promotion — reused across cmd lists
     * within one frame. Lifetime is this function. */
    static uint32_t *idx_stage = NULL;
    static size_t   idx_stage_cap = 0;

    size_t vtx_off = 0;
    size_t idx_off_u32 = 0;
    size_t n = 0;

    for (uint32_t li = 0; li < fh->cmd_list_count; li++) {
        if (n >= cls_max) break;

        const struct ymgui_wire_cmd_list *clh =
            (const struct ymgui_wire_cmd_list *)cur;
        cur += sizeof(*clh);

        const uint8_t *vtx = cur;
        size_t vbytes = (size_t)clh->vtx_count * 20u;
        cur += vbytes;

        const uint8_t *idx = cur;
        size_t ibytes_padded = (size_t)clh->idx_count * idx_bpe;
        if (ibytes_padded & 3u) ibytes_padded += 4u - (ibytes_padded & 3u);
        cur += ibytes_padded;

        const struct ymgui_wire_cmd *cmds = (const struct ymgui_wire_cmd *)cur;
        cur += (size_t)clh->cmd_count * sizeof(struct ymgui_wire_cmd);
        if (cur > end) return 0;

        /* Vertex data goes straight to GPU. */
        if (vbytes)
            wgpuQueueWriteBuffer(l->queue, l->vtx_buf, vtx_off, vtx, vbytes);

        /* Index promotion u16 → u32 (no-op if already u32). */
        size_t i32_bytes = (size_t)clh->idx_count * 4u;
        if (i32_bytes) {
            if (idx32) {
                wgpuQueueWriteBuffer(l->queue, l->idx_buf, idx_off_u32 * 4u,
                                     idx, i32_bytes);
            } else {
                if (idx_stage_cap < clh->idx_count) {
                    free(idx_stage);
                    idx_stage = (uint32_t *)malloc(
                        (size_t)clh->idx_count * sizeof(uint32_t));
                    idx_stage_cap = clh->idx_count;
                    if (!idx_stage) return 0;
                }
                const uint16_t *src = (const uint16_t *)idx;
                for (uint32_t i = 0; i < clh->idx_count; i++)
                    idx_stage[i] = src[i];
                wgpuQueueWriteBuffer(l->queue, l->idx_buf, idx_off_u32 * 4u,
                                     idx_stage, i32_bytes);
            }
        }

        cls[n].vtx_byte_offset = vtx_off;
        cls[n].idx_u32_offset  = idx_off_u32;
        cls[n].cmd_count       = clh->cmd_count;
        cls[n].cmds            = cmds;
        cls[n].vtx_count       = clh->vtx_count;
        n++;

        vtx_off     += vbytes;
        idx_off_u32 += clh->idx_count;
    }
    *cls_count = n;
    return 1;
}

static struct yetty_ycore_void_result
ymgui_render(struct yetty_yterm_terminal_layer *self,
             struct yetty_yrender_target *target)
{
    struct yetty_yterm_ymgui_layer *l = (struct yetty_yterm_ymgui_layer *)self;
    if (!target || !target->ops || !target->ops->get_view)
        return YETTY_ERR(yetty_ycore_void, "ymgui: target has no get_view");

    /* Pipeline is built on first render and never recompiled. */
    if (!l->pipeline_ready) {
        if (!build_pipeline(l))
            return YETTY_ERR(yetty_ycore_void, "ymgui: pipeline build failed");
        rebuild_bind_group(l);  /* may still be NULL if no atlas yet */
    }

    WGPUTextureView view = target->ops->get_view(target);
    if (!view)
        return YETTY_ERR(yetty_ycore_void, "ymgui: target view is NULL");

    /* Visibility test — anchor row inside the visible window. */
    uint32_t row0 = l->row0_absolute;
    uint32_t rows = l->base.grid_size.rows;
    int visible = l->has_frame &&
        l->frame_rolling_row + frame_row_span(l) > row0 &&
        l->frame_rolling_row < row0 + rows;

    /* No frame / no atlas yet → just clear the layer's texture so the
     * compositor sees transparent for this layer. */
    if (!visible || !l->atlas_ready || !l->bind_group) {
        WGPUCommandEncoderDescriptor ed = {0};
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(l->device, &ed);
        WGPURenderPassColorAttachment ca = {0};
        ca.view       = view;
        ca.loadOp     = WGPULoadOp_Clear;
        ca.storeOp    = WGPUStoreOp_Store;
        ca.clearValue = (WGPUColor){0, 0, 0, 0};
        ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        WGPURenderPassDescriptor pd = {0};
        pd.colorAttachmentCount = 1;
        pd.colorAttachments     = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pd);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUCommandBufferDescriptor cd = {0};
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, &cd);
        wgpuQueueSubmit(l->queue, 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(enc);
        self->dirty = 0;
        return YETTY_OK_VOID();
    }

    /* Measure the wire frame and grow GPU buffers if needed. */
    struct draw_pass_inputs dp;
    int idx32 = 0;
    if (!frame_measure(l, &dp, &idx32))
        return YETTY_ERR(yetty_ycore_void, "ymgui: frame layout invalid");

    if (dp.total_vtx_bytes == 0 || dp.total_idx_bytes == 0) {
        self->dirty = 0;
        return YETTY_OK_VOID();
    }

    if (!ensure_buffer(l->device, &l->vtx_buf, &l->vtx_buf_capacity,
                       dp.total_vtx_bytes,
                       WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst))
        return YETTY_ERR(yetty_ycore_void, "ymgui: vtx buffer alloc failed");
    if (!ensure_buffer(l->device, &l->idx_buf, &l->idx_buf_capacity,
                       dp.total_idx_bytes,
                       WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst))
        return YETTY_ERR(yetty_ycore_void, "ymgui: idx buffer alloc failed");

    /* Stream verts/idx to GPU. Up to 16 cmd lists per frame is plenty —
     * ImGui typically emits 1 list per top-level window. */
    enum { MAX_CL = 32 };
    struct cl_offsets cls[MAX_CL];
    size_t cls_count = 0;
    if (!frame_upload(l, cls, MAX_CL, &cls_count, idx32))
        return YETTY_ERR(yetty_ycore_void, "ymgui: frame upload failed");

    /* Uniforms — display size for NDC + scroll offset for the vertex shader. */
    float frame_top_y = (float)((int32_t)l->frame_rolling_row -
                                (int32_t)row0) * l->base.cell_size.height;
    float uniforms[8] = {
        l->frame_display_w, l->frame_display_h,   /* display_size  */
        0.0f, frame_top_y,                         /* frame_top     */
        0.0f, 0.0f, 0.0f, 0.0f,                    /* pad to 32 B   */
    };
    wgpuQueueWriteBuffer(l->queue, l->uniform_buffer, 0,
                         uniforms, sizeof(uniforms));

    /* Encode the render pass. */
    WGPUCommandEncoderDescriptor ed = {0};
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(l->device, &ed);

    WGPURenderPassColorAttachment ca = {0};
    ca.view       = view;
    ca.loadOp     = WGPULoadOp_Clear;
    ca.storeOp    = WGPUStoreOp_Store;
    ca.clearValue = (WGPUColor){0, 0, 0, 0};
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    WGPURenderPassDescriptor pd = {0};
    pd.colorAttachmentCount = 1;
    pd.colorAttachments     = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pd);

    wgpuRenderPassEncoderSetPipeline(pass, l->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, l->bind_group, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, l->vtx_buf, 0, dp.total_vtx_bytes);
    wgpuRenderPassEncoderSetIndexBuffer(pass, l->idx_buf,
                                        WGPUIndexFormat_Uint32, 0,
                                        dp.total_idx_bytes);

    /* Iterate the cmd lists × cmds. ImGui's cmd carries a clip rect (in
     * frame-local pixel space) plus vtx_offset / idx_offset into the
     * cmd-list-local arrays — convert to GPU-buffer global offsets. */
    float W = l->frame_display_w, H = l->frame_display_h;
    for (size_t i = 0; i < cls_count; i++) {
        const struct cl_offsets *cl = &cls[i];
        uint32_t base_vtx_idx = (uint32_t)(cl->vtx_byte_offset / 20u);
        for (uint32_t c = 0; c < cl->cmd_count; c++) {
            const struct ymgui_wire_cmd *dc = &cl->cmds[c];
            if (dc->elem_count == 0) continue;

            /* Scissor — clamp to layer dimensions and to non-negative. */
            float sx0 = dc->clip_min_x, sy0 = dc->clip_min_y;
            float sx1 = dc->clip_max_x, sy1 = dc->clip_max_y;
            if (sx0 < 0) sx0 = 0;       if (sy0 < 0) sy0 = 0;
            if (sx1 > W) sx1 = W;       if (sy1 > H) sy1 = H;
            if (sx1 <= sx0 || sy1 <= sy0) continue;
            wgpuRenderPassEncoderSetScissorRect(pass,
                (uint32_t)sx0, (uint32_t)sy0,
                (uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0));

            wgpuRenderPassEncoderDrawIndexed(pass,
                dc->elem_count,
                /*instanceCount*/ 1,
                /*firstIndex   */ (uint32_t)cl->idx_u32_offset + dc->idx_offset,
                /*baseVertex   */ (int32_t)(base_vtx_idx + dc->vtx_offset),
                /*firstInstance*/ 0);
        }
    }

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBufferDescriptor cd = {0};
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, &cd);
    wgpuQueueSubmit(l->queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);

    self->dirty = 0;
    return YETTY_OK_VOID();
}

/*===========================================================================
 * Misc ops — input ignored, scroll bumps row0, set_cursor latches anchor.
 *=========================================================================*/

static int ymgui_is_empty(const struct yetty_yterm_terminal_layer *self)
{
    const struct yetty_yterm_ymgui_layer *l =
        (const struct yetty_yterm_ymgui_layer *)self;
    if (!l->has_frame) return 1;
    uint32_t span = frame_row_span(l);
    if (l->frame_rolling_row + span <= l->row0_absolute) return 1;
    if (l->frame_rolling_row >=
        l->row0_absolute + l->base.grid_size.rows) return 1;
    return 0;
}

static int ymgui_on_key (struct yetty_yterm_terminal_layer *self, int key, int mods)
{ (void)self; (void)key; (void)mods; return 0; }

static int ymgui_on_char(struct yetty_yterm_terminal_layer *self,
                         uint32_t cp, int mods)
{ (void)self; (void)cp; (void)mods; return 0; }

static struct yetty_ycore_void_result
ymgui_scroll(struct yetty_yterm_terminal_layer *self, int lines)
{
    struct yetty_yterm_ymgui_layer *l = (struct yetty_yterm_ymgui_layer *)self;
    if (lines <= 0) return YETTY_OK_VOID();
    l->row0_absolute += (uint32_t)lines;
    self->dirty = 1;
    return YETTY_OK_VOID();
}

static void ymgui_set_cursor(struct yetty_yterm_terminal_layer *self,
                             int col, int row)
{
    struct yetty_yterm_ymgui_layer *l = (struct yetty_yterm_ymgui_layer *)self;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    l->cursor_col = (uint32_t)col;
    l->cursor_row = (uint32_t)row;
}
