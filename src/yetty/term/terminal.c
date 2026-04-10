#include <yetty/term/terminal.h>
#include <yetty/term/text-layer.h>
#include <yetty/core/event-loop.h>
#include <yetty/core/event.h>
#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/pty-poll-source.h>
#include <yetty/render/gpu-resource-set.h>
#include <yetty/render/gpu-allocator.h>
#include <yetty/render/gpu-resource-binder.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define YETTY_TERM_TERMINAL_MAX_LAYERS 256

struct yetty_term_terminal {
    struct yetty_core_event_listener listener;  /* must be first for container_of */
    struct yetty_term_terminal_context context;
    uint32_t cols;
    uint32_t rows;
    struct yetty_term_terminal_layer *layers[YETTY_TERM_TERMINAL_MAX_LAYERS];
    size_t layer_count;
    yetty_core_poll_id pty_poll_id;
    struct yetty_render_gpu_allocator *allocator;
    struct yetty_render_gpu_resource_binder *binder;
};

/* Forward declarations */
static void terminal_read_pty(struct yetty_term_terminal *terminal);
static struct yetty_core_void_result terminal_render_frame(struct yetty_term_terminal *terminal);

/* Event handler */
static int terminal_event_handler(
    struct yetty_core_event_listener *listener,
    const struct yetty_core_event *event)
{
    struct yetty_term_terminal *terminal = (struct yetty_term_terminal *)listener;

    switch (event->type) {
    case YETTY_EVENT_RENDER: {
        struct yetty_core_void_result res = terminal_render_frame(terminal);
        if (!YETTY_IS_OK(res))
            yerror("terminal: render_frame failed: %s", res.error.msg);
        return 1;
    }

    case YETTY_EVENT_POLL_READABLE:
        ydebug("terminal: POLL_READABLE event fd=%d", event->poll.fd);
        terminal_read_pty(terminal);
        return 1;

    default:
        return 0;
    }
}

/* Render a frame */
static struct yetty_core_void_result terminal_render_frame(struct yetty_term_terminal *terminal)
{
    struct yetty_gpu_context *gpu = &terminal->context.yetty_context.gpu_context;
    struct yetty_app_gpu_context *app_gpu = &gpu->app_gpu_context;

    ydebug("terminal_render_frame: starting");

    if (!terminal->binder) {
        yerror("terminal_render_frame: no binder");
        return YETTY_ERR(yetty_core_void, "no binder");
    }

    /* Submit resource sets from all layers */
    for (size_t i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        if (layer && layer->ops && layer->ops->get_gpu_resource_set) {
            struct yetty_render_gpu_resource_set_result rs_res = layer->ops->get_gpu_resource_set(layer);
            if (!YETTY_IS_OK(rs_res)) {
                yerror("terminal_render_frame: layer %zu get_gpu_resource_set failed: %s", i, rs_res.error.msg);
                return YETTY_ERR(yetty_core_void, rs_res.error.msg);
            }
            terminal->binder->ops->submit(terminal->binder, rs_res.value);
        }
    }

    /* Finalize (compile shaders, create pipeline if needed) */
    struct yetty_core_void_result res = terminal->binder->ops->finalize(terminal->binder);
    if (!YETTY_IS_OK(res)) {
        yerror("terminal_render_frame: finalize failed: %s", res.error.msg);
        return res;
    }

    /* Get surface texture */
    WGPUSurfaceTexture surface_texture;
    wgpuSurfaceGetCurrentTexture(app_gpu->surface, &surface_texture);
    if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        yerror("terminal_render_frame: surface texture not ready, status=%d", surface_texture.status);
        return YETTY_ERR(yetty_core_void, "surface texture not ready");
    }

    WGPUTextureView target_view = wgpuTextureCreateView(surface_texture.texture, NULL);
    if (!target_view) {
        yerror("terminal_render_frame: failed to create texture view");
        wgpuTextureRelease(surface_texture.texture);
        return YETTY_ERR(yetty_core_void, "failed to create texture view");
    }

    /* Create command encoder */
    WGPUCommandEncoderDescriptor enc_desc = {0};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(gpu->device, &enc_desc);

    /* Begin render pass */
    WGPURenderPassColorAttachment color_attachment = {0};
    color_attachment.view = target_view;
    color_attachment.loadOp = WGPULoadOp_Clear;
    color_attachment.storeOp = WGPUStoreOp_Store;
    color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 1.0};

    WGPURenderPassDescriptor pass_desc = {0};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);

    /* Bind and draw */
    WGPURenderPipeline pipeline = terminal->binder->ops->get_pipeline(terminal->binder);
    WGPUBuffer quad_vb = terminal->binder->ops->get_quad_vertex_buffer(terminal->binder);
    if (pipeline && quad_vb) {
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        terminal->binder->ops->bind(terminal->binder, pass, 0);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quad_vb, 0, 6 * 4 * sizeof(float));
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
    }

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    /* Submit */
    WGPUCommandBufferDescriptor cmd_desc = {0};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    wgpuQueueSubmit(gpu->queue, 1, &cmd);

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(target_view);
    wgpuSurfacePresent(app_gpu->surface);

    ydebug("terminal_render_frame: done");
    return YETTY_OK_VOID();
}

/* Read from PTY and feed to text layer */
static void terminal_read_pty(struct yetty_term_terminal *terminal)
{
    struct yetty_platform_pty *pty = terminal->context.pty;
    char buf[4096];
    int dirty = 0;

    if (!pty || !pty->ops || !pty->ops->read)
        return;

    struct yetty_core_size_result res;
    while ((res = pty->ops->read(pty, buf, sizeof(buf))), YETTY_IS_OK(res) && res.value > 0) {
        /* Feed to text layer */
        if (terminal->layer_count > 0) {
            struct yetty_term_terminal_layer *layer = terminal->layers[0];
            if (layer && layer->ops && layer->ops->write) {
                layer->ops->write(layer, buf, res.value);
                dirty = 1;
            }
        }
    }

    /* Request render if text layer is dirty */
    if (dirty && terminal->layer_count > 0) {
        struct yetty_term_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->dirty) {
            terminal->context.event_loop->ops->request_render(terminal->context.event_loop);
        }
    }
}

/* Terminal creation/destruction */

struct yetty_term_terminal_result yetty_term_terminal_create(
    uint32_t cols, uint32_t rows,
    const struct yetty_context *yetty_context)
{
    struct yetty_term_terminal *terminal;
    struct yetty_core_void_result res;

    ydebug("terminal_create: cols=%u rows=%u", cols, rows);

    terminal = calloc(1, sizeof(struct yetty_term_terminal));
    if (!terminal)
        return YETTY_ERR(yetty_term_terminal, "failed to allocate terminal");

    terminal->listener.handler = terminal_event_handler;
    terminal->cols = cols;
    terminal->rows = rows;
    terminal->layer_count = 0;
    terminal->context.yetty_context = *yetty_context;

    /* Create event loop */
    struct yetty_platform_input_pipe *pipe = yetty_context->app_context.platform_input_pipe;
    struct yetty_core_event_loop_result event_loop_res = yetty_core_event_loop_create(pipe);
    if (!YETTY_IS_OK(event_loop_res)) {
        ydebug("terminal_create: failed to create event loop");
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create event loop");
    }
    terminal->context.event_loop = event_loop_res.value;
    ydebug("terminal_create: event_loop created at %p", (void *)terminal->context.event_loop);

    /* Register for render events */
    res = terminal->context.event_loop->ops->register_listener(
        terminal->context.event_loop, YETTY_EVENT_RENDER, &terminal->listener, 0);
    if (!YETTY_IS_OK(res)) {
        ydebug("terminal_create: failed to register render listener");
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to register render listener");
    }
    ydebug("terminal_create: registered for RENDER events");

    /* Create PTY */
    struct yetty_platform_pty_factory *pty_factory = yetty_context->app_context.pty_factory;
    if (pty_factory && pty_factory->ops && pty_factory->ops->create_pty) {
        struct yetty_platform_pty_result pty_res = pty_factory->ops->create_pty(pty_factory);
        if (YETTY_IS_OK(pty_res)) {
            terminal->context.pty = pty_res.value;
            ydebug("terminal_create: PTY created at %p", (void *)terminal->context.pty);

            /* Set up PTY poll */
            struct yetty_platform_pty_poll_source *poll_source = terminal->context.pty->ops->poll_source(terminal->context.pty);
            if (poll_source) {
                struct yetty_core_poll_id_result poll_res = terminal->context.event_loop->ops->create_pty_poll(
                    terminal->context.event_loop, poll_source);
                if (YETTY_IS_OK(poll_res)) {
                    terminal->pty_poll_id = poll_res.value;
                    terminal->context.event_loop->ops->register_poll_listener(
                        terminal->context.event_loop, terminal->pty_poll_id, &terminal->listener);
                    terminal->context.event_loop->ops->start_poll(
                        terminal->context.event_loop, terminal->pty_poll_id, YETTY_CORE_POLL_READABLE);
                    ydebug("terminal_create: PTY poll started");
                }
            }
        } else {
            ydebug("terminal_create: failed to create PTY (non-fatal)");
        }
    }

    /* Create text layer */
    struct yetty_term_terminal_layer_result text_layer_res = yetty_term_terminal_text_layer_create(cols, rows, yetty_context);
    if (!YETTY_IS_OK(text_layer_res)) {
        ydebug("terminal_create: failed to create text layer");
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create text layer");
    }
    yetty_term_terminal_layer_add(terminal, text_layer_res.value);
    ydebug("terminal_create: text_layer created and added");

    /* Create GPU allocator */
    struct yetty_render_gpu_allocator_result alloc_res =
        yetty_render_gpu_allocator_create(yetty_context->gpu_context.device);
    if (!YETTY_IS_OK(alloc_res)) {
        ydebug("terminal_create: failed to create gpu allocator");
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create gpu allocator");
    }
    terminal->allocator = alloc_res.value;
    ydebug("terminal_create: gpu allocator created");

    /* Create GPU resource binder */
    struct yetty_render_gpu_resource_binder_result binder_res =
        yetty_render_gpu_resource_binder_create(
            yetty_context->gpu_context.device,
            yetty_context->gpu_context.queue,
            yetty_context->gpu_context.surface_format,
            terminal->allocator);
    if (!YETTY_IS_OK(binder_res)) {
        ydebug("terminal_create: failed to create gpu resource binder");
        terminal->allocator->ops->destroy(terminal->allocator);
        if (terminal->context.pty)
            terminal->context.pty->ops->destroy(terminal->context.pty);
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);
        free(terminal);
        return YETTY_ERR(yetty_term_terminal, "failed to create gpu resource binder");
    }
    terminal->binder = binder_res.value;
    ydebug("terminal_create: gpu resource binder created");

    return YETTY_OK(yetty_term_terminal, terminal);
}

void yetty_term_terminal_destroy(struct yetty_term_terminal *terminal)
{
    size_t i;

    if (!terminal)
        return;

    if (terminal->binder && terminal->binder->ops && terminal->binder->ops->destroy)
        terminal->binder->ops->destroy(terminal->binder);

    if (terminal->allocator && terminal->allocator->ops && terminal->allocator->ops->destroy)
        terminal->allocator->ops->destroy(terminal->allocator);

    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        if (layer && layer->ops && layer->ops->destroy)
            layer->ops->destroy(layer);
    }

    if (terminal->context.pty && terminal->context.pty->ops && terminal->context.pty->ops->destroy)
        terminal->context.pty->ops->destroy(terminal->context.pty);

    if (terminal->context.event_loop && terminal->context.event_loop->ops && terminal->context.event_loop->ops->destroy)
        terminal->context.event_loop->ops->destroy(terminal->context.event_loop);

    free(terminal);
}

struct yetty_core_void_result yetty_term_terminal_run(struct yetty_term_terminal *terminal)
{
    ydebug("terminal_run: Starting...");

    if (!terminal) {
        ydebug("terminal_run: terminal is null!");
        return YETTY_ERR(yetty_core_void, "terminal is null");
    }

    if (!terminal->context.event_loop) {
        ydebug("terminal_run: terminal has no event_loop!");
        return YETTY_ERR(yetty_core_void, "terminal has no event_loop");
    }

    if (!terminal->context.event_loop->ops) {
        ydebug("terminal_run: event_loop has no ops!");
        return YETTY_ERR(yetty_core_void, "event_loop has no ops");
    }

    if (!terminal->context.event_loop->ops->start) {
        ydebug("terminal_run: event_loop has no start op!");
        return YETTY_ERR(yetty_core_void, "event_loop has no start op");
    }

    ydebug("terminal_run: Calling event_loop start...");
    struct yetty_core_void_result res = terminal->context.event_loop->ops->start(terminal->context.event_loop);
    ydebug("terminal_run: event_loop start returned, ok=%d", YETTY_IS_OK(res));

    if (!YETTY_IS_OK(res)) {
        return res;
    }

    return YETTY_OK_VOID();
}

/* Terminal input */

void yetty_term_terminal_write(struct yetty_term_terminal *terminal,
                               const char *data, size_t len)
{
    if (!terminal || !data || len == 0)
        return;

    /* Send to first layer (text layer) */
    if (terminal->layer_count > 0) {
        struct yetty_term_terminal_layer *layer = terminal->layers[0];
        if (layer && layer->ops && layer->ops->write) {
            layer->ops->write(layer, data, len);
            ydebug("terminal_write: sent %zu bytes to text layer", len);
        }
    }
}

void yetty_term_terminal_resize(struct yetty_term_terminal *terminal,
                                uint32_t cols, uint32_t rows)
{
    size_t i;

    if (!terminal)
        return;

    terminal->cols = cols;
    terminal->rows = rows;

    for (i = 0; i < terminal->layer_count; i++) {
        struct yetty_term_terminal_layer *layer = terminal->layers[i];
        if (layer && layer->ops && layer->ops->resize)
            layer->ops->resize(layer, cols, rows);
    }
}

/* Terminal state */

uint32_t yetty_term_terminal_get_cols(const struct yetty_term_terminal *terminal)
{
    return terminal ? terminal->cols : 0;
}

uint32_t yetty_term_terminal_get_rows(const struct yetty_term_terminal *terminal)
{
    return terminal ? terminal->rows : 0;
}

/* Layer management */

void yetty_term_terminal_layer_add(struct yetty_term_terminal *terminal,
                                   struct yetty_term_terminal_layer *layer)
{
    if (!terminal || !layer)
        return;

    if (terminal->layer_count >= YETTY_TERM_TERMINAL_MAX_LAYERS)
        return;

    terminal->layers[terminal->layer_count++] = layer;
}

void yetty_term_terminal_layer_remove(struct yetty_term_terminal *terminal,
                                      struct yetty_term_terminal_layer *layer)
{
    size_t i;

    if (!terminal || !layer)
        return;

    for (i = 0; i < terminal->layer_count; i++) {
        if (terminal->layers[i] == layer) {
            memmove(&terminal->layers[i], &terminal->layers[i + 1],
                    (terminal->layer_count - i - 1) * sizeof(terminal->layers[0]));
            terminal->layer_count--;
            return;
        }
    }
}

size_t yetty_term_terminal_layer_count(const struct yetty_term_terminal *terminal)
{
    return terminal ? terminal->layer_count : 0;
}

struct yetty_term_terminal_layer *yetty_term_terminal_layer_get(
    const struct yetty_term_terminal *terminal, size_t index)
{
    if (!terminal || index >= terminal->layer_count)
        return NULL;

    return terminal->layers[index];
}
