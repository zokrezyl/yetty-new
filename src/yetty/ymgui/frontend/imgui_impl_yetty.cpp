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

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define YMGUI_STDOUT_FD 1
#else
#include <unistd.h>
#define YMGUI_STDOUT_FD STDOUT_FILENO
#endif

/* Sanity: our wire vertex layout MUST match ImDrawVert. */
#if !defined(IMGUI_USE_BGRA_PACKED_COLOR)
/* ImDrawVert is {ImVec2 pos, ImVec2 uv, ImU32 col} = 20 bytes */
#endif

struct ymgui_impl_state {
    int               out_fd;
    struct ymgui_buf  scratch;  /* reused every frame */
    int               atlas_uploaded;
};

static struct ymgui_impl_state g_state = { YMGUI_STDOUT_FD, { 0, 0, 0 }, 0 };

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
