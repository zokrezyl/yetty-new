/*
 * ymgui/wire.h — binary wire format shared by frontend (C++) and backend (C).
 *
 * All envelopes are carried via yface (see <yetty/yface/yface.h>):
 *
 *     \e]<code>;<flag>;<base64[(LZ4F)payload]>\e\\
 *
 * The OSC <code> identifies the message type — there are no verbs in the
 * body. Codes in the 600000-range flow client→server (frame, texture,
 * clear); 700000-range flows server→client (input events).
 *
 * The single character right after the first ';' is the compression flag:
 * '0' for raw b64, '1' for LZ4F+b64. Compression is on for big payloads
 * (frames, textures, future video) and off for short events (mouse, resize).
 *
 * All integers are little-endian. All structs are naturally aligned at 4 B.
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

/*=============================================================================
 * OSC codes
 *
 * Allocation policy: 6xxxxx = client→server, 7xxxxx = server→client. Within
 * each direction the code itself discriminates the message type, so dispatch
 * is a single switch on osc_code with no body inspection.
 *===========================================================================*/

/* client → server (frontend → ymgui-layer) */
#define YMGUI_OSC_CS_FRAME      600000  /* ymgui_wire_frame, compressed=1 */
#define YMGUI_OSC_CS_TEX        600001  /* ymgui_wire_tex,   compressed=1 */
#define YMGUI_OSC_CS_CLEAR      600002  /* empty body,       compressed=0 */

/* server → client (yetty terminal → frontend / ygui / yrich) */
#define YMGUI_OSC_SC_MOUSE      700000  /* ymgui_wire_input_mouse,  comp=0 */
#define YMGUI_OSC_SC_RESIZE     700001  /* ymgui_wire_input_resize, comp=0 */

/*=============================================================================
 * Magic numbers + versioning
 *===========================================================================*/
#define YMGUI_WIRE_MAGIC_FRAME        0x4D47494Fu  /* 'OIGM' → "YMGI" */
#define YMGUI_WIRE_MAGIC_TEX          0x4D58544Fu  /* 'OTXM' → "YMTX" */
#define YMGUI_WIRE_MAGIC_INPUT_MOUSE  0x4D49534Du  /* "MSIM" reversed: "MISM" */
#define YMGUI_WIRE_MAGIC_INPUT_RESIZE 0x4D52534Du  /* "MSRM" reversed: "MRSM" */

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
 * Frame header — payload of YMGUI_OSC_CS_FRAME.
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
 * Texture upload payload of YMGUI_OSC_CS_TEX. Pixel data follows the
 * header, length = width*height*bpp (1 = R8, 4 = RGBA8).
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

/*=============================================================================
 * Input events (server → client)
 *===========================================================================*/

/* ymgui_wire_input_mouse.kind */
enum ymgui_wire_input_mouse_kind {
    YMGUI_INPUT_MOUSE_POS    = 0,  /* x,y; buttons_held mask for drag tracking */
    YMGUI_INPUT_MOUSE_BUTTON = 1,  /* button transition: button + pressed + x,y */
    YMGUI_INPUT_MOUSE_WHEEL  = 2,  /* wheel: wheel_dy at x,y */
};

/* Single struct covers move / button / wheel — kind discriminates.
 * Fields not relevant to a kind are zero on the wire. */
struct ymgui_wire_input_mouse {
    uint32_t magic;           /* YMGUI_WIRE_MAGIC_INPUT_MOUSE */
    uint32_t version;         /* YMGUI_WIRE_VERSION */
    uint32_t kind;            /* enum ymgui_wire_input_mouse_kind */
    int32_t  button;          /* BUTTON: 0=left,1=right,2=middle,...; else -1 */
    int32_t  pressed;         /* BUTTON: 1=down 0=up; else 0 */
    uint32_t buttons_held;    /* POS during drag: bitmask (1<<button) */
    float    x;
    float    y;
    float    wheel_dy;        /* WHEEL */
    uint32_t _pad0;
};

struct ymgui_wire_input_resize {
    uint32_t magic;           /* YMGUI_WIRE_MAGIC_INPUT_RESIZE */
    uint32_t version;         /* YMGUI_WIRE_VERSION */
    float    width;           /* pixels */
    float    height;          /* pixels */
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YMGUI_WIRE_H */
