#ifndef YETTY_YTERM_YMGUI_LAYER_H
#define YETTY_YTERM_YMGUI_LAYER_H

#include <stdint.h>
#include <yetty/yterm/terminal.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ymgui layer — renders a Dear ImGui frame delivered over OSC vendor 666680.
 *
 * Semantics mirror ypaint-layer's scrolling mode:
 *   - The frame is anchored at the cursor row at the moment --frame arrives.
 *   - The anchor row is stored as a rolling_row (monotonic counter, same
 *     model the ypaint canvas uses), so scrolling is O(1): a uniform offset
 *     tells the fragment shader where the frame sits relative to the top
 *     visible row.
 *   - If the frame needs more rows than remain below the cursor, the layer
 *     propagates `scroll_fn(rows_needed)` so the terminal scrolls up before
 *     the frame is shown — exactly what a shell does when a tall command
 *     prints near the bottom of the screen.
 *   - When the anchor row scrolls off the top, the layer reports is_empty()
 *     and the renderer skips it.
 *
 * Wire format: see include/yetty/ymgui/wire.h. Three OSC verbs are honoured:
 *   --frame;<base64(ymgui_wire_frame)>   replace current frame
 *   --tex;<base64(ymgui_wire_tex)>       upload / replace a texture (atlas)
 *   --clear                              drop current frame
 *
 * Create as scrolling_mode=1. (Overlay mode is not yet wired — ImGui's
 * native model is cursor-anchored, terminal-scrolling output.)
 */

struct yetty_yterm_terminal_layer_result yetty_yterm_ymgui_layer_create(
    uint32_t cols, uint32_t rows,
    float cell_width, float cell_height,
    const struct yetty_context *context,
    yetty_yterm_request_render_fn request_render_fn,
    void *request_render_userdata,
    yetty_yterm_scroll_fn scroll_fn,
    void *scroll_userdata,
    yetty_yterm_cursor_fn cursor_fn,
    void *cursor_userdata);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YTERM_YMGUI_LAYER_H */
