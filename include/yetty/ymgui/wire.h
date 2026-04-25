/*
 * ymgui/wire.h — binary wire format shared by frontend (C++) and backend (C).
 *
 * Carried over OSC vendor 666680 as base64'd payloads. Three verbs:
 *
 *   \e]666680;--frame;<base64(ymgui_wire_frame)>\e\\
 *   \e]666680;--tex;<base64(ymgui_wire_tex)>\e\\
 *   \e]666680;--clear\e\\
 *
 * All integers are little-endian. All structs are naturally aligned at 4 bytes.
 * Vertex layout matches Dear ImGui's ImDrawVert exactly: {pos,uv,col} = 20 B.
 * Index type is 16-bit (ImDrawIdx default); if frontend is built with
 * `#define ImDrawIdx unsigned int` the flag YMGUI_FRAME_FLAG_IDX32 is set.
 */

#ifndef YETTY_YMGUI_WIRE_H
#define YETTY_YMGUI_WIRE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic numbers (little-endian ASCII). */
#define YMGUI_WIRE_MAGIC_FRAME  0x4D47494Fu  /* 'OIGM' → "YMGI" */
#define YMGUI_WIRE_MAGIC_TEX    0x4D58544Fu  /* 'OTXM' → "YMTX" */

#define YMGUI_WIRE_VERSION      1u

/* Texture IDs. The frontend uses the ImGuiIO font atlas as tex 1.
 * User textures (v2) would allocate 2..N. tex_id=0 = "no texture"
 * (solid-colored triangles only). */
#define YMGUI_TEX_ID_NONE       0u
#define YMGUI_TEX_ID_FONT_ATLAS 1u

/* Texture formats. */
#define YMGUI_TEX_FMT_R8        1u
#define YMGUI_TEX_FMT_RGBA8     2u

/* Frame flags. */
#define YMGUI_FRAME_FLAG_IDX32  (1u << 0)

/*---------------------------------------------------------------------------
 * Vertex — identical to ImDrawVert (pos:vec2, uv:vec2, col:u32 RGBA).
 *-------------------------------------------------------------------------*/
struct ymgui_wire_vertex {
    float pos_x;
    float pos_y;
    float uv_x;
    float uv_y;
    uint32_t col;  /* IM_COL32 (AABBGGRR little-endian → R,G,B,A bytes) */
};

/*---------------------------------------------------------------------------
 * Per-draw-call command.
 *-------------------------------------------------------------------------*/
struct ymgui_wire_cmd {
    float clip_min_x;
    float clip_min_y;
    float clip_max_x;
    float clip_max_y;
    uint32_t tex_id;
    uint32_t vtx_offset;  /* within the owning cmd-list's vertex array */
    uint32_t idx_offset;  /* within the owning cmd-list's index  array */
    uint32_t elem_count;  /* number of indices for this call (multiple of 3) */
};

/*---------------------------------------------------------------------------
 * Per-cmd-list header. Followed immediately by:
 *     struct ymgui_wire_vertex vtx[vtx_count];
 *     uint16_t (or uint32_t) idx[idx_count];       // padded to 4 bytes
 *     struct ymgui_wire_cmd    cmds[cmd_count];
 *-------------------------------------------------------------------------*/
struct ymgui_wire_cmd_list {
    uint32_t vtx_count;
    uint32_t idx_count;
    uint32_t cmd_count;
    uint32_t _pad0;
};

/*---------------------------------------------------------------------------
 * Frame header — starts every --frame payload.
 *-------------------------------------------------------------------------*/
struct ymgui_wire_frame {
    uint32_t magic;           /* YMGUI_WIRE_MAGIC_FRAME */
    uint32_t version;         /* YMGUI_WIRE_VERSION */
    uint32_t flags;           /* YMGUI_FRAME_FLAG_* */
    uint32_t total_size;      /* bytes in the whole frame payload */
    float    display_pos_x;   /* ImDrawData::DisplayPos */
    float    display_pos_y;
    float    display_size_x;  /* ImDrawData::DisplaySize (logical pixels) */
    float    display_size_y;
    float    fb_scale_x;      /* ImDrawData::FramebufferScale */
    float    fb_scale_y;
    uint32_t cmd_list_count;
    uint32_t _pad0;
    /* Followed by cmd_list_count × (ymgui_wire_cmd_list + vtx + idx + cmds). */
};

/*---------------------------------------------------------------------------
 * Texture upload payload. Pixel data follows the header, length = width*height*bpp.
 * bpp = 1 (R8) or 4 (RGBA8).
 *-------------------------------------------------------------------------*/
struct ymgui_wire_tex {
    uint32_t magic;           /* YMGUI_WIRE_MAGIC_TEX */
    uint32_t version;         /* YMGUI_WIRE_VERSION */
    uint32_t tex_id;
    uint32_t format;          /* YMGUI_TEX_FMT_* */
    uint32_t width;
    uint32_t height;
    uint32_t total_size;      /* bytes in the whole tex payload */
    uint32_t _pad0;
    /* Followed by width*height*bpp bytes of pixel data. */
};

/* OSC vendor identifier for ymgui. Distinct from ygui (666674) and the
 * ypaint-overlay slot (666675). */
#define YMGUI_OSC_VENDOR     "666680"
#define YMGUI_OSC_VENDOR_INT 666680

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMGUI_WIRE_H */
