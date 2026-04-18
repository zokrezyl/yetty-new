#ifndef YETTY_TERM_PTY_READER_H
#define YETTY_TERM_PTY_READER_H

#include <yetty/ycore/result.h>
#include <yetty/platform/pty.h>
#include <yetty/yterm/terminal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Yetty OSC vendor IDs */
#define YETTY_OSC_YPAINT_SCROLL  666674
#define YETTY_OSC_YPAINT_OVERLAY 666675

struct yetty_term_pty_reader;

struct yetty_term_pty_reader_result yetty_term_pty_reader_create(
    struct yetty_platform_pty *pty);

void yetty_term_pty_reader_destroy(struct yetty_term_pty_reader *reader);

void yetty_term_pty_reader_register_default_sink(
    struct yetty_term_pty_reader *reader,
    struct yetty_term_terminal_layer *layer);

void yetty_term_pty_reader_register_osc_sink(
    struct yetty_term_pty_reader *reader,
    int vendor_id,
    struct yetty_term_terminal_layer *layer);

int yetty_term_pty_reader_read(struct yetty_term_pty_reader *reader);

YETTY_RESULT_DECLARE(yetty_term_pty_reader, struct yetty_term_pty_reader *);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_TERM_PTY_READER_H */
