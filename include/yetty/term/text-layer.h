#ifndef YETTY_TERM_TEXT_LAYER_H
#define YETTY_TERM_TEXT_LAYER_H

#include <stdint.h>
#include <yetty/term/terminal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text layer - renders terminal text via libvterm */
struct yetty_term_terminal_layer_result yetty_term_terminal_text_layer_create(
    uint32_t cols, uint32_t rows,
    const struct yetty_context *context,
    yetty_term_pty_write_fn pty_write_fn,
    void *pty_write_userdata,
    yetty_term_request_render_fn request_render_fn,
    void *request_render_userdata,
    yetty_term_scroll_fn scroll_fn,
    void *scroll_userdata,
    yetty_term_cursor_fn cursor_fn,
    void *cursor_userdata);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_TERM_TEXT_LAYER_H */
