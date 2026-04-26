/*
 * yplatform/term.h - Cross-platform terminal helpers
 */

#ifndef YETTY_YPLATFORM_TERM_H
#define YETTY_YPLATFORM_TERM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check if stderr supports ANSI colors (enables VT processing on Windows) */
int yplatform_stderr_supports_color(void);

/* Format current local time as "HH:MM:SS.mmm" into buf (must be >= 16 bytes) */
void yplatform_format_timestamp(char *buf, size_t bufsize);

/* Blocking write of `len` bytes to stdout. Loops past short writes and
 * EINTR. Returns 0 on success, -1 on hard error. */
int yplatform_stdout_write(const void *data, size_t len);

/* Switch stdin into raw/cbreak mode (no echo, no line buffering, no signal
 * generation). The original mode is restored automatically at process exit
 * via atexit(). Idempotent. */
void yplatform_stdin_raw_mode_enable(void);

/* Wait up to `timeout_ms` milliseconds for stdin to become readable.
 * Returns 1 if readable, 0 on timeout, -1 on error. */
int yplatform_stdin_wait_readable(int timeout_ms);

/* Read up to `max_len` bytes from stdin (non-blocking after a successful
 * wait_readable). Returns the byte count, 0 on EOF, or -1 on error. */
int yplatform_stdin_read(void *buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YPLATFORM_TERM_H */
