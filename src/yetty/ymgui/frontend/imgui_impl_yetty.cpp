/*
 * imgui_impl_yetty.cpp — Minimum C++ shim over C helpers.
 *
 * The only reason this is C++ is that Dear ImGui's API is C++. Everything
 * heavy (buffer growth, base64, OSC framing) lives in the C helpers.
 * This file is pure C syntax compiled as C++, plus direct calls into
 * ImGui's namespaced API.
 */

#include "imgui_impl_yetty.h"
#include "ymgui_encode.h"

#include <yetty/ymgui/wire.h>

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

/* Sanity: our wire vertex layout MUST match ImDrawVert. */
#if !defined(IMGUI_USE_BGRA_PACKED_COLOR)
/* ImDrawVert is {ImVec2 pos, ImVec2 uv, ImU32 col} = 20 bytes */
#endif

struct ymgui_impl_state {
    int               out_fd;
    struct ymgui_buf  scratch;       /* reused every frame */
    int               atlas_uploaded;

    /* Platform / input state */
    int               in_fd;
    int               raw_mode_active;
#ifndef _WIN32
    struct termios    saved_termios;
#endif
    /* Re-entrant OSC parser working buffer. We drain stdin into here every
     * PollInput, scan for complete sequences, and slide consumed bytes
     * out — partial trailing bytes wait for the next call. */
    char              parse_buf[4096];
    size_t            parse_len;

    /* Latest input state, copied into ImGuiIO at NewFrame-time. */
    float             cursor_x;
    float             cursor_y;
    int               buttons_held;   /* OR of (1<<button), per OSC 777778 */
    bool              buttons_down[5];
    float             wheel_dy;       /* accumulated since last frame */
    float             display_w;
    float             display_h;
    int               display_known;
};

static struct ymgui_impl_state g_state = {
    YMGUI_STDOUT_FD, { 0, 0, 0 }, 0,
    YMGUI_STDIN_FD, 0,
#ifndef _WIN32
    {},
#endif
    {0}, 0,
    -1.0f, -1.0f, 0, {false, false, false, false, false}, 0.0f,
    0.0f, 0.0f, 0,
};

/*===========================================================================
 * Public API
 *=========================================================================*/

void ImGui_ImplYetty_SetOutputFd(int fd)
{
    g_state.out_fd = fd;
}

bool ImGui_ImplYetty_UploadFontAtlas(void)
{
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = NULL;
    int w = 0, h = 0;
    /* Alpha8 is compact on the wire and enough for the default font. */
    io.Fonts->GetTexDataAsAlpha8(&pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0)
        return false;

    size_t pixel_bytes = (size_t)w * (size_t)h; /* R8 */
    size_t payload_sz  = sizeof(struct ymgui_wire_tex) + pixel_bytes;

    struct ymgui_buf buf;
    ymgui_buf_init(&buf);
    if (ymgui_buf_reserve(&buf, payload_sz) != 0)
        return false;

    struct ymgui_wire_tex* hdr = (struct ymgui_wire_tex*)
        ymgui_buf_alloc(&buf, sizeof(*hdr));
    hdr->magic      = YMGUI_WIRE_MAGIC_TEX;
    hdr->version    = YMGUI_WIRE_VERSION;
    hdr->tex_id     = YMGUI_TEX_ID_FONT_ATLAS;
    hdr->format     = YMGUI_TEX_FMT_R8;
    hdr->width      = (uint32_t)w;
    hdr->height     = (uint32_t)h;
    hdr->total_size = (uint32_t)payload_sz;
    hdr->_pad0      = 0;

    ymgui_buf_write(&buf, pixels, pixel_bytes);

    int rc = ymgui_osc_write(g_state.out_fd, "--tex", buf.data, buf.size);
    ymgui_buf_free(&buf);

    if (rc != 0)
        return false;

    io.Fonts->SetTexID((ImTextureID)(intptr_t)YMGUI_TEX_ID_FONT_ATLAS);
    g_state.atlas_uploaded = 1;
    return true;
}

bool ImGui_ImplYetty_Init(void)
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName     = "imgui_impl_yetty";
    io.BackendRendererUserData = &g_state;
    /* We do not need VtxOffset because we pack per-cmd-list anyway, but it
     * doesn't hurt and lets ImGui avoid splitting draw lists. */
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    ymgui_buf_init(&g_state.scratch);

    return ImGui_ImplYetty_UploadFontAtlas();
}

void ImGui_ImplYetty_Shutdown(void)
{
    ymgui_buf_free(&g_state.scratch);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName     = NULL;
    io.BackendRendererUserData = NULL;
    g_state.atlas_uploaded = 0;
}

void ImGui_ImplYetty_NewFrame(void)
{
    if (!g_state.atlas_uploaded)
        ImGui_ImplYetty_UploadFontAtlas();
}

void ImGui_ImplYetty_Clear(void)
{
    ymgui_osc_write(g_state.out_fd, "--clear", NULL, 0);
}

/*===========================================================================
 * Render — serialize ImDrawData into wire format, base64, OSC.
 *=========================================================================*/

void ImGui_ImplYetty_RenderDrawData(ImDrawData* draw_data)
{
    if (!draw_data || draw_data->CmdListsCount <= 0)
        return;

    /* Static-size sanity — we memcpy ImDrawVert straight onto the wire. */
    if (sizeof(ImDrawVert) != sizeof(struct ymgui_wire_vertex))
        return;

    struct ymgui_buf* buf = &g_state.scratch;
    ymgui_buf_reset(buf);

    /* Frame header — total_size patched at the end. */
    struct ymgui_wire_frame* fh = (struct ymgui_wire_frame*)
        ymgui_buf_alloc(buf, sizeof(*fh));
    if (!fh) return;

    fh->magic          = YMGUI_WIRE_MAGIC_FRAME;
    fh->version        = YMGUI_WIRE_VERSION;
    fh->flags          = (sizeof(ImDrawIdx) == 4) ? YMGUI_FRAME_FLAG_IDX32 : 0;
    fh->total_size     = 0; /* patched at end */
    fh->display_pos_x  = draw_data->DisplayPos.x;
    fh->display_pos_y  = draw_data->DisplayPos.y;
    fh->display_size_x = draw_data->DisplaySize.x;
    fh->display_size_y = draw_data->DisplaySize.y;
    fh->fb_scale_x     = draw_data->FramebufferScale.x;
    fh->fb_scale_y     = draw_data->FramebufferScale.y;
    fh->cmd_list_count = (uint32_t)draw_data->CmdListsCount;
    fh->_pad0          = 0;

    /* Walking the cmd lists can realloc the buffer, so grab the header
     * offset instead of holding the pointer across calls. */
    size_t frame_header_off = 0;

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cl = draw_data->CmdLists[n];

        struct ymgui_wire_cmd_list cl_hdr;
        cl_hdr.vtx_count = (uint32_t)cl->VtxBuffer.Size;
        cl_hdr.idx_count = (uint32_t)cl->IdxBuffer.Size;
        cl_hdr.cmd_count = (uint32_t)cl->CmdBuffer.Size;
        cl_hdr._pad0     = 0;

        if (ymgui_buf_write(buf, &cl_hdr, sizeof(cl_hdr)) != 0)
            return;

        /* Vertices — layout-compatible memcpy. */
        if (cl_hdr.vtx_count) {
            size_t nbytes = (size_t)cl_hdr.vtx_count * sizeof(ImDrawVert);
            if (ymgui_buf_write(buf, cl->VtxBuffer.Data, nbytes) != 0)
                return;
        }

        /* Indices. */
        if (cl_hdr.idx_count) {
            size_t nbytes = (size_t)cl_hdr.idx_count * sizeof(ImDrawIdx);
            if (ymgui_buf_write(buf, cl->IdxBuffer.Data, nbytes) != 0)
                return;
            /* Pad to 4-byte alignment so the cmd array is aligned. */
            if (ymgui_buf_align(buf, 4) != 0)
                return;
        }

        /* Cmds — re-emit into wire struct (ImDrawCmd has callbacks/padding we
         * don't want to send). */
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd* dc = &cl->CmdBuffer[c];
            if (dc->UserCallback)
                continue; /* TODO: support ImDrawCallback_ResetRenderState */

            struct ymgui_wire_cmd wc;
            wc.clip_min_x = dc->ClipRect.x;
            wc.clip_min_y = dc->ClipRect.y;
            wc.clip_max_x = dc->ClipRect.z;
            wc.clip_max_y = dc->ClipRect.w;
            wc.tex_id     = (uint32_t)(intptr_t)dc->GetTexID();
            wc.vtx_offset = dc->VtxOffset;
            wc.idx_offset = dc->IdxOffset;
            wc.elem_count = dc->ElemCount;

            if (ymgui_buf_write(buf, &wc, sizeof(wc)) != 0)
                return;
        }
    }

    /* Patch total_size now that the buffer is finalised. */
    struct ymgui_wire_frame* fh_final =
        (struct ymgui_wire_frame*)(buf->data + frame_header_off);
    fh_final->total_size = (uint32_t)buf->size;

    ymgui_osc_write(g_state.out_fd, "--frame", buf->data, buf->size);
}

/*===========================================================================
 * Platform side — raw stdin, DEC ?1500/?1501 subscription, OSC parser
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
    /* Non-blocking so PollInput never stalls. */
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

    /* Stdout non-blocking too — ymgui_osc_write parks any unsent tail in a
     * pending queue on EAGAIN instead of blocking the loop. The CHILD pty
     * end (/dev/pts/N) is shared between all of stdin/stdout/stderr in the
     * default fork-pty model, so flipping O_NONBLOCK on stdout flips it
     * for all three. That's actually what we want — both sides of our io
     * are non-blocking. The kernel-level termios is still raw from above. */
    {
        int flags = fcntl(g_state.out_fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(g_state.out_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Subscribe: \e[?1500h \e[?1501h. ymgui-layer's text-layer settermprop
     * hook flips the per-terminal subscription latch; rising edge also
     * triggers a OSC 777780 pixel-size emission so we get DisplaySize
     * before the first event arrives. */
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

/*---------------------------------------------------------------------------
 * OSC parser — minimal scanner for our three verbs. Returns the number of
 * bytes consumed, or 0 if a complete sequence is not yet available at the
 * head of the buffer (caller should keep it for next time).
 *
 * Recognised:
 *   \e]777777;<card>;<btn>;<press>;<x>;<y>;<scroll-dy>\e\\
 *   \e]777778;<card>;<btn-held>;<x>;<y>\e\\
 *   \e]777780;<w>;<h>\e\\
 *
 * Anything else is consumed as one byte (so we keep advancing through
 * stray bytes from the PTY). The terminator is ESC '\\' (ST).
 *-------------------------------------------------------------------------*/
static size_t parse_osc(const char* buf, size_t len)
{
    if (len < 2) return 0;
    if (buf[0] != '\x1b') return 1;
    if (buf[1] != ']')   return 1;

    /* Find ST = ESC '\\' */
    size_t end = 0;
    for (size_t i = 2; i + 1 < len; i++) {
        if (buf[i] == '\x1b' && buf[i + 1] == '\\') {
            end = i;
            break;
        }
    }
    if (end == 0) return 0; /* incomplete */

    /* Body: buf[2..end). Parse vendor code. */
    size_t i = 2;
    int code = 0;
    while (i < end && buf[i] >= '0' && buf[i] <= '9') {
        code = code * 10 + (buf[i] - '0');
        i++;
    }
    if (i >= end || buf[i] != ';') return end + 2; /* unknown — skip */
    i++;

    auto parse_float = [&](float* out) -> bool {
        bool neg = false;
        if (i < end && buf[i] == '-') { neg = true; i++; }
        float f = 0.0f;
        bool any = false;
        while (i < end && buf[i] >= '0' && buf[i] <= '9') {
            f = f * 10.0f + (float)(buf[i] - '0');
            i++; any = true;
        }
        if (i < end && buf[i] == '.') {
            i++;
            float frac = 0.1f;
            while (i < end && buf[i] >= '0' && buf[i] <= '9') {
                f += (float)(buf[i] - '0') * frac;
                frac *= 0.1f;
                i++; any = true;
            }
        }
        *out = neg ? -f : f;
        return any;
    };
    auto parse_int = [&](int* out) -> bool {
        bool neg = false;
        if (i < end && buf[i] == '-') { neg = true; i++; }
        int v = 0;
        bool any = false;
        while (i < end && buf[i] >= '0' && buf[i] <= '9') {
            v = v * 10 + (buf[i] - '0');
            i++; any = true;
        }
        *out = neg ? -v : v;
        return any;
    };
    auto eat_sep = [&]() -> bool {
        if (i >= end || buf[i] != ';') return false;
        i++;
        return true;
    };
    auto skip_field = [&]() {
        while (i < end && buf[i] != ';') i++;
    };

    /* All input goes via the ImGuiIO event queue (AddMouse*Event), not by
     * clobbering MousePos/MouseDown directly — otherwise a press+release
     * landing in the same PollInput collapses to "no click", because
     * ImGui only ever sees the final state of the frame. The queue
     * preserves transitions even when multiple events arrive between
     * NewFrames. */
    ImGuiIO& io = ImGui::GetIO();

    if (code == 777777) {
        /* card; btn; press; x; y; [scroll-dy] */
        skip_field(); if (!eat_sep()) return end + 2;
        int btn = 0, press = 0;
        float x = 0, y = 0, dy = 0;
        if (!parse_int(&btn))   return end + 2; if (!eat_sep()) return end + 2;
        if (!parse_int(&press)) return end + 2; if (!eat_sep()) return end + 2;
        if (!parse_float(&x))   return end + 2; if (!eat_sep()) return end + 2;
        if (!parse_float(&y))   return end + 2;
        if (i < end && buf[i] == ';') {
            i++;
            (void)parse_float(&dy);
        }
        fprintf(stderr,
                "[ymgui-frontend] OSC 777777 btn=%d press=%d xy=(%.1f,%.1f) dy=%.1f\n",
                btn, press, x, y, dy);
        io.AddMousePosEvent(x, y);
        if (dy != 0.0f) {
            io.AddMouseWheelEvent(0.0f, dy);
        } else if (btn >= 0 && btn < 5) {
            io.AddMouseButtonEvent(btn, press != 0);
        }
    } else if (code == 777778) {
        skip_field(); if (!eat_sep()) return end + 2;
        int held = 0;
        float x = 0, y = 0;
        if (!parse_int(&held)) return end + 2; if (!eat_sep()) return end + 2;
        if (!parse_float(&x))  return end + 2; if (!eat_sep()) return end + 2;
        if (!parse_float(&y))  return end + 2;
        static int s_move_count = 0;
        if (++s_move_count % 30 == 1) {
            fprintf(stderr, "[ymgui-frontend] OSC 777778 mv #%d xy=(%.1f,%.1f)\n",
                    s_move_count, x, y);
        }
        io.AddMousePosEvent(x, y);
        (void)held;  /* ImGui derives drag state from the per-button events */
    } else if (code == 777780) {
        float w = 0, h = 0;
        if (!parse_float(&w)) return end + 2; if (!eat_sep()) return end + 2;
        if (!parse_float(&h)) return end + 2;
        fprintf(stderr, "[ymgui-frontend] OSC 777780 w=%.0f h=%.0f\n", w, h);
        if (w > 0.0f && h > 0.0f) {
            g_state.display_w = w;
            g_state.display_h = h;
            g_state.display_known = 1;
        }
    } else {
        fprintf(stderr, "[ymgui-frontend] OSC %d (ignored)\n", code);
    }

    return end + 2; /* consumed up to and including ST */
}

void ImGui_ImplYetty_PollInput(void)
{
#ifdef _WIN32
    return;
#else
    if (!g_state.raw_mode_active) return;

    /* Drain stdin into parse_buf. */
    for (;;) {
        if (g_state.parse_len >= sizeof(g_state.parse_buf)) {
            /* Buffer full and we couldn't make progress — drop oldest half. */
            size_t keep = sizeof(g_state.parse_buf) / 2;
            memmove(g_state.parse_buf,
                    g_state.parse_buf + (g_state.parse_len - keep), keep);
            g_state.parse_len = keep;
        }
        ssize_t n = read(g_state.in_fd,
                         g_state.parse_buf + g_state.parse_len,
                         sizeof(g_state.parse_buf) - g_state.parse_len);
        if (n <= 0) break;
        g_state.parse_len += (size_t)n;
    }

    /* Parse complete sequences from the head; keep the trailing partial. */
    size_t off = 0;
    while (off < g_state.parse_len) {
        size_t consumed = parse_osc(g_state.parse_buf + off,
                                    g_state.parse_len - off);
        if (consumed == 0) break; /* need more data */
        off += consumed;
    }
    if (off > 0 && off < g_state.parse_len) {
        memmove(g_state.parse_buf, g_state.parse_buf + off,
                g_state.parse_len - off);
        g_state.parse_len -= off;
    } else if (off >= g_state.parse_len) {
        g_state.parse_len = 0;
    }

    /* Mouse pos/button/wheel are fed via Add*Event in the parser. The only
     * thing we still set here is DisplaySize — it's a config value, not
     * an event. */
    ImGuiIO& io = ImGui::GetIO();
    if (g_state.display_known)
        io.DisplaySize = ImVec2(g_state.display_w, g_state.display_h);
#endif
}

/*===========================================================================
 * Idle-mode helpers: WaitInput + DrawDataHash
 *=========================================================================*/

bool ImGui_ImplYetty_WaitInput(int timeout_ms)
{
#ifdef _WIN32
    /* Windows: PollInput is a no-op anyway. Sleep the timeout. */
    if (timeout_ms > 0) {
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return false;
#else
    /* Already-buffered input from a previous PollInput counts — return
     * true immediately so the caller drains it without blocking. */
    if (g_state.parse_len > 0) return true;

    /* If we have an OSC tail queued for stdout, also wake on POLLOUT so
     * we can drain it as soon as the kernel buffer has space — even if
     * no new input arrives. */
    struct pollfd pfd[2];
    int nfds = 1;
    pfd[0].fd      = g_state.in_fd;
    pfd[0].events  = POLLIN;
    pfd[0].revents = 0;
    if (ymgui_osc_pending()) {
        pfd[1].fd      = g_state.out_fd;
        pfd[1].events  = POLLOUT;
        pfd[1].revents = 0;
        nfds = 2;
    }
    int n = poll(pfd, (nfds_t)nfds, timeout_ms);
    if (n <= 0) return false;

    /* Drain the OSC tail if stdout is now write-ready. We do this here
     * (not in PollInput) so a tail-only wakeup doesn't get treated as
     * a UI event by the demo loop. */
    if (nfds == 2 && (pfd[1].revents & POLLOUT))
        ymgui_osc_flush(g_state.out_fd);

    return (pfd[0].revents & POLLIN) != 0;
#endif
}

