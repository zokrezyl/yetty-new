/*
 * imgui_impl_yetty.cpp — Dear ImGui ↔ yetty bridge.
 *
 * The renderer half encodes ImDrawData into ymgui_wire_* structs and ships
 * them through yface (LZ4F + b64) over the OSC envelope. The platform half
 * sets stdin to raw mode, subscribes to yetty's pixel-precise mouse, and
 * routes incoming OSC events into ImGuiIO via the GLFW-style On* hooks.
 *
 * The OSC envelope scanner lives in yface — both the sync PollInput path
 * and the async ymgui_event_loop hand bytes to yetty_yface_feed_bytes()
 * and the yface fires typed callbacks per envelope.
 */

#include "imgui_impl_yetty.h"
#include "ymgui_encode.h"

#include <yetty/yclient-lib/event-loop.h>
#include <yetty/ymgui/wire.h>
#include <yetty/yface/yface.h>
#include <yetty/ycore/types.h>

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define YMGUI_STDOUT_FD 1
#define YMGUI_STDIN_FD  0
#else
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#define YMGUI_STDOUT_FD STDOUT_FILENO
#define YMGUI_STDIN_FD  STDIN_FILENO
#endif

struct ymgui_impl_state {
    int                  out_fd;
    struct yetty_yface  *yface_out;       /* OSC envelope + LZ4F + b64 streamer */
    struct yetty_yface  *yface_in;        /* stream-scanner for incoming events */
    int                  atlas_uploaded;

    /* Platform / input state */
    int               in_fd;
    int               raw_mode_active;
#ifndef _WIN32
    struct termios    saved_termios;
#endif

    /* Cached display size from the last YMGUI_OSC_SC_RESIZE. */
    float             display_w;
    float             display_h;
    int               display_known;
};

static struct ymgui_impl_state g_state = {
    YMGUI_STDOUT_FD, nullptr, nullptr, 0,
    YMGUI_STDIN_FD, 0,
#ifndef _WIN32
    {},
#endif
    0.0f, 0.0f, 0,
};

/* Push everything yface_out accumulated to the wire via the non-blocking
 * pending-write helper, then reset out_buf. Returns the same tri-state as
 * ymgui_pending_write. */
static int flush_yface_to_fd(void)
{
    if (!g_state.yface_out) return -1;
    struct yetty_ycore_buffer *out = yetty_yface_out_buf(g_state.yface_out);
    if (!out || out->size == 0) return 0;
    int rc = ymgui_pending_write(g_state.out_fd, out->data, out->size);
    yetty_ycore_buffer_clear(out);
    return rc;
}

/*===========================================================================
 * Outgoing — Init / Shutdown / atlas / clear / RenderDrawData
 *=========================================================================*/

void ImGui_ImplYetty_SetOutputFd(int fd) { g_state.out_fd = fd; }

bool ImGui_ImplYetty_UploadFontAtlas(void)
{
    if (!g_state.yface_out) return false;

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    /* Alpha8 — small on the wire, enough for the default font. */
    io.Fonts->GetTexDataAsAlpha8(&pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0) return false;

    size_t pixel_bytes = (size_t)w * (size_t)h; /* R8 */
    size_t payload_sz  = sizeof(struct ymgui_wire_tex) + pixel_bytes;

    struct ymgui_wire_tex hdr = {};
    hdr.magic      = YMGUI_WIRE_MAGIC_TEX;
    hdr.version    = YMGUI_WIRE_VERSION;
    hdr.tex_id     = YMGUI_TEX_ID_FONT_ATLAS;
    hdr.format     = YMGUI_TEX_FMT_R8;
    hdr.width      = (uint32_t)w;
    hdr.height     = (uint32_t)h;
    hdr.total_size = (uint32_t)payload_sz;

    if (!yetty_yface_start_write(g_state.yface_out,
                                 YMGUI_OSC_CS_TEX, /*compressed=*/1, /*args=*/NULL, /*args_len=*/0).ok)
        return false;
    if (!yetty_yface_write(g_state.yface_out, &hdr, sizeof(hdr)).ok) return false;
    if (!yetty_yface_write(g_state.yface_out, pixels, pixel_bytes).ok) return false;
    if (!yetty_yface_finish_write(g_state.yface_out).ok) return false;

    if (flush_yface_to_fd() < 0) return false;

    io.Fonts->SetTexID((ImTextureID)(intptr_t)YMGUI_TEX_ID_FONT_ATLAS);
    g_state.atlas_uploaded = 1;
    return true;
}

bool ImGui_ImplYetty_Init(void)
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName     = "imgui_impl_yetty";
    io.BackendRendererUserData = &g_state;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    /* Two yface instances — one each for the encode/decode pipelines.
     * Sharing one is technically possible but would forbid concurrent
     * read-and-write states. */
    {
        struct yetty_yface_ptr_result yr = yetty_yface_create();
        if (!yr.ok) return false;
        g_state.yface_out = yr.value;
    }
    {
        struct yetty_yface_ptr_result yr = yetty_yface_create();
        if (!yr.ok) {
            yetty_yface_destroy(g_state.yface_out);
            g_state.yface_out = nullptr;
            return false;
        }
        g_state.yface_in = yr.value;
    }

    return ImGui_ImplYetty_UploadFontAtlas();
}

void ImGui_ImplYetty_Shutdown(void)
{
    if (g_state.yface_out) {
        yetty_yface_destroy(g_state.yface_out);
        g_state.yface_out = nullptr;
    }
    if (g_state.yface_in) {
        yetty_yface_destroy(g_state.yface_in);
        g_state.yface_in = nullptr;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName     = nullptr;
    io.BackendRendererUserData = nullptr;
    g_state.atlas_uploaded = 0;
}

void ImGui_ImplYetty_NewFrame(void)
{
    if (!g_state.atlas_uploaded)
        ImGui_ImplYetty_UploadFontAtlas();
}

void ImGui_ImplYetty_Clear(void)
{
    if (!g_state.yface_out) return;
    /* Empty body, raw (no LZ4 — nothing to compress). */
    if (!yetty_yface_start_write(g_state.yface_out,
                                 YMGUI_OSC_CS_CLEAR, /*compressed=*/0, /*args=*/NULL, /*args_len=*/0).ok) return;
    if (!yetty_yface_finish_write(g_state.yface_out).ok) return;
    flush_yface_to_fd();
}

/*===========================================================================
 * Render — serialize ImDrawData into wire format, base64, OSC.
 *=========================================================================*/

void ImGui_ImplYetty_RenderDrawData(ImDrawData* draw_data)
{
    if (!draw_data || draw_data->CmdListsCount <= 0) return;
    if (!g_state.yface_out) return;
    /* Static-size sanity — we memcpy ImDrawVert straight onto the wire. */
    if (sizeof(ImDrawVert) != sizeof(struct ymgui_wire_vertex)) return;

    /* Compute total payload size up-front so the frame header carries an
     * accurate total_size (used for validation on the receiver). The
     * walk-twice cost is negligible vs. the actual encode + write. */
    size_t total_size = sizeof(struct ymgui_wire_frame);
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cl = draw_data->CmdLists[n];
        size_t idx_bytes = (size_t)cl->IdxBuffer.Size * sizeof(ImDrawIdx);
        if (idx_bytes & 3u) idx_bytes += 4u - (idx_bytes & 3u);
        total_size += sizeof(struct ymgui_wire_cmd_list)
                    + (size_t)cl->VtxBuffer.Size * sizeof(ImDrawVert)
                    + idx_bytes
                    + (size_t)cl->CmdBuffer.Size * sizeof(struct ymgui_wire_cmd);
    }

    struct ymgui_wire_frame fh = {};
    fh.magic          = YMGUI_WIRE_MAGIC_FRAME;
    fh.version        = YMGUI_WIRE_VERSION;
    fh.flags          = (sizeof(ImDrawIdx) == 4) ? YMGUI_FRAME_FLAG_IDX32 : 0;
    fh.total_size     = (uint32_t)total_size;
    fh.display_pos_x  = draw_data->DisplayPos.x;
    fh.display_pos_y  = draw_data->DisplayPos.y;
    fh.display_size_x = draw_data->DisplaySize.x;
    fh.display_size_y = draw_data->DisplaySize.y;
    fh.fb_scale_x     = draw_data->FramebufferScale.x;
    fh.fb_scale_y     = draw_data->FramebufferScale.y;
    fh.cmd_list_count = (uint32_t)draw_data->CmdListsCount;

    if (!yetty_yface_start_write(g_state.yface_out,
                                 YMGUI_OSC_CS_FRAME, /*compressed=*/1, /*args=*/NULL, /*args_len=*/0).ok) return;
    if (!yetty_yface_write(g_state.yface_out, &fh, sizeof(fh)).ok) return;

    static const uint8_t pad[4] = {0, 0, 0, 0};

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cl = draw_data->CmdLists[n];

        struct ymgui_wire_cmd_list cl_hdr = {};
        cl_hdr.vtx_count = (uint32_t)cl->VtxBuffer.Size;
        cl_hdr.idx_count = (uint32_t)cl->IdxBuffer.Size;
        cl_hdr.cmd_count = (uint32_t)cl->CmdBuffer.Size;
        if (!yetty_yface_write(g_state.yface_out, &cl_hdr, sizeof(cl_hdr)).ok) return;

        if (cl_hdr.vtx_count) {
            size_t nbytes = (size_t)cl_hdr.vtx_count * sizeof(ImDrawVert);
            if (!yetty_yface_write(g_state.yface_out, cl->VtxBuffer.Data, nbytes).ok)
                return;
        }
        if (cl_hdr.idx_count) {
            size_t nbytes = (size_t)cl_hdr.idx_count * sizeof(ImDrawIdx);
            if (!yetty_yface_write(g_state.yface_out, cl->IdxBuffer.Data, nbytes).ok)
                return;
            /* Pad to 4 bytes so the cmd array starts aligned. */
            size_t rem = nbytes & 3u;
            if (rem) {
                if (!yetty_yface_write(g_state.yface_out, pad, 4u - rem).ok)
                    return;
            }
        }

        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd* dc = &cl->CmdBuffer[c];
            if (dc->UserCallback) continue; /* TODO: ImDrawCallback_ResetRenderState */
            struct ymgui_wire_cmd wc = {};
            wc.clip_min_x = dc->ClipRect.x;
            wc.clip_min_y = dc->ClipRect.y;
            wc.clip_max_x = dc->ClipRect.z;
            wc.clip_max_y = dc->ClipRect.w;
            wc.tex_id     = (uint32_t)(intptr_t)dc->GetTexID();
            wc.vtx_offset = dc->VtxOffset;
            wc.idx_offset = dc->IdxOffset;
            wc.elem_count = dc->ElemCount;
            if (!yetty_yface_write(g_state.yface_out, &wc, sizeof(wc)).ok) return;
        }
    }

    if (!yetty_yface_finish_write(g_state.yface_out).ok) return;
    flush_yface_to_fd();
}

/*===========================================================================
 * Platform / input — raw stdin, DEC ?1500/?1501 subscription
 *=========================================================================*/

void ImGui_ImplYetty_SetInputFd(int fd) { g_state.in_fd = fd; }

#ifndef _WIN32
static bool platform_set_raw_mode(int fd, struct termios* saved)
{
    if (tcgetattr(fd, saved) != 0)
        return false;
    struct termios raw = *saved;
    /* cfmakeraw is convenient but we want to keep ISIG so Ctrl-C still
     * works for the user as a way to abort the demo program. */
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR
                     | ICRNL | IXON);
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return false;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

static void platform_restore_termios(int fd, const struct termios* saved)
{
    tcsetattr(fd, TCSANOW, saved);
}
#endif

bool ImGui_ImplYetty_PlatformInit(void)
{
#ifdef _WIN32
    /* Windows console raw mode is a separate beast; defer to v2. */
    return false;
#else
    if (!platform_set_raw_mode(g_state.in_fd, &g_state.saved_termios))
        return false;
    g_state.raw_mode_active = 1;

    /* Stdout non-blocking too — ymgui_pending_write parks any unsent tail
     * in a queue on EAGAIN instead of blocking. The CHILD pty end is
     * shared between stdin/stdout/stderr, so flipping O_NONBLOCK on
     * stdout flips it for all three. That's actually what we want — both
     * sides of our io are non-blocking. The kernel-level termios is
     * still raw from above. */
    {
        int flags = fcntl(g_state.out_fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(g_state.out_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Subscribe: \e[?1500h \e[?1501h. yetty's text-layer settermprop hook
     * flips the per-terminal subscription latch; rising edge also triggers
     * a YMGUI_OSC_SC_RESIZE emission so we get DisplaySize before the
     * first event arrives. */
    static const char subscribe[] = "\033[?1500h\033[?1501h";
    ssize_t w = write(g_state.out_fd, subscribe, sizeof(subscribe) - 1);
    (void)w;

    /* ImGui needs to see SOME mouse position before the first frame or it
     * won't hover anything. Off-screen until we hear from yetty. */
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    return true;
#endif
}

void ImGui_ImplYetty_PlatformShutdown(void)
{
#ifndef _WIN32
    if (g_state.raw_mode_active) {
        static const char unsubscribe[] = "\033[?1500l\033[?1501l";
        ssize_t w = write(g_state.out_fd, unsubscribe, sizeof(unsubscribe) - 1);
        (void)w;
        platform_restore_termios(g_state.in_fd, &g_state.saved_termios);
        g_state.raw_mode_active = 0;
    }
#endif
}

/*===========================================================================
 * Push-input hooks — feed events into ImGuiIO from anywhere
 *
 * All three input paths (sync PollInput, async event loop, manual app
 * code) funnel through these so the ImGuiIO calls happen in exactly one
 * place. The renderer is independent of how events arrive.
 *=========================================================================*/

void ImGui_ImplYetty_OnMousePos(double x, double y, uint32_t buttons_held)
{
    (void)buttons_held;  /* ImGui derives drag from per-button events */
    ImGui::GetIO().AddMousePosEvent((float)x, (float)y);
}

void ImGui_ImplYetty_OnMouseButton(int button, int pressed,
                                   double x, double y)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent((float)x, (float)y);
    if (button >= 0 && button < 5)
        io.AddMouseButtonEvent(button, pressed != 0);
}

void ImGui_ImplYetty_OnMouseWheel(double dy, double x, double y)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent((float)x, (float)y);
    io.AddMouseWheelEvent(0.0f, (float)dy);
}

void ImGui_ImplYetty_OnResize(double width, double height)
{
    if (width <= 0.0 || height <= 0.0) return;
    g_state.display_w     = (float)width;
    g_state.display_h     = (float)height;
    g_state.display_known = 1;
    ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
}

/*===========================================================================
 * Sync PollInput — drains stdin through yface
 *=========================================================================*/

extern "C" {
static void poll_on_osc(void *user, int osc_code,
                        const uint8_t *args,    size_t args_len,
                        const uint8_t *payload, size_t len)
{
    (void)user; (void)args; (void)args_len;  /* mouse/resize have no args */
    switch (osc_code) {
    case YMGUI_OSC_SC_MOUSE: {
        if (len < sizeof(struct ymgui_wire_input_mouse)) return;
        const struct ymgui_wire_input_mouse *m =
            (const struct ymgui_wire_input_mouse *)payload;
        if (m->magic != YMGUI_WIRE_MAGIC_INPUT_MOUSE) return;
        switch (m->kind) {
        case YMGUI_INPUT_MOUSE_POS:
            ImGui_ImplYetty_OnMousePos(m->x, m->y, m->buttons_held);
            break;
        case YMGUI_INPUT_MOUSE_BUTTON:
            ImGui_ImplYetty_OnMouseButton(m->button, m->pressed, m->x, m->y);
            break;
        case YMGUI_INPUT_MOUSE_WHEEL:
            ImGui_ImplYetty_OnMouseWheel(m->wheel_dy, m->x, m->y);
            break;
        }
        break;
    }
    case YMGUI_OSC_SC_RESIZE: {
        if (len < sizeof(struct ymgui_wire_input_resize)) return;
        const struct ymgui_wire_input_resize *r =
            (const struct ymgui_wire_input_resize *)payload;
        if (r->magic != YMGUI_WIRE_MAGIC_INPUT_RESIZE) return;
        ImGui_ImplYetty_OnResize(r->width, r->height);
        break;
    }
    default:
        break;
    }
}
}  /* extern "C" */

void ImGui_ImplYetty_PollInput(void)
{
#ifdef _WIN32
    return;
#else
    if (!g_state.raw_mode_active || !g_state.yface_in) return;

    /* yface_in's handlers are wired once on first call. */
    static int handlers_set = 0;
    if (!handlers_set) {
        yetty_yface_set_handlers(g_state.yface_in, poll_on_osc, nullptr, &g_state);
        handlers_set = 1;
    }

    char buf[4096];
    for (;;) {
        ssize_t n = read(g_state.in_fd, buf, sizeof(buf));
        if (n <= 0) break;
        yetty_yface_feed_bytes(g_state.yface_in, buf, (size_t)n);
    }
#endif
}

bool ImGui_ImplYetty_WaitInput(int timeout_ms)
{
#ifdef _WIN32
    if (timeout_ms > 0) {
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
    }
    return false;
#else
    /* If we have an OSC tail queued for stdout, also wake on POLLOUT so
     * we can drain it as soon as the kernel buffer has space — even if
     * no new input arrives. */
    struct pollfd pfd[2];
    int nfds = 1;
    pfd[0].fd      = g_state.in_fd;
    pfd[0].events  = POLLIN;
    pfd[0].revents = 0;
    if (ymgui_pending_active()) {
        pfd[1].fd      = g_state.out_fd;
        pfd[1].events  = POLLOUT;
        pfd[1].revents = 0;
        nfds = 2;
    }
    int n = poll(pfd, (nfds_t)nfds, timeout_ms);
    if (n <= 0) return false;

    if (nfds == 2 && (pfd[1].revents & POLLOUT))
        ymgui_pending_flush(g_state.out_fd);

    return (pfd[0].revents & POLLIN) != 0;
#endif
}

/*===========================================================================
 * Async — bridge ymgui_event_loop callbacks into ImGuiIO
 *=========================================================================*/

extern "C" {
static void loop_on_pos   (void *u, double x, double y, uint32_t held) {
    (void)u; ImGui_ImplYetty_OnMousePos(x, y, held);
}
static void loop_on_btn   (void *u, int b, int p, double x, double y) {
    (void)u; ImGui_ImplYetty_OnMouseButton(b, p, x, y);
}
static void loop_on_wheel (void *u, double dy, double x, double y) {
    (void)u; ImGui_ImplYetty_OnMouseWheel(dy, x, y);
}
static void loop_on_resize(void *u, double w, double h) {
    (void)u; ImGui_ImplYetty_OnResize(w, h);
}
}  /* extern "C" */

void ImGui_ImplYetty_AttachEventLoop(struct yetty_yclient_event_loop *loop)
{
    if (!loop) return;
    yetty_yclient_event_loop_set_user           (loop, &g_state);
    yetty_yclient_event_loop_set_mouse_pos_cb   (loop, loop_on_pos);
    yetty_yclient_event_loop_set_mouse_button_cb(loop, loop_on_btn);
    yetty_yclient_event_loop_set_mouse_wheel_cb (loop, loop_on_wheel);
    yetty_yclient_event_loop_set_resize_cb      (loop, loop_on_resize);
}
