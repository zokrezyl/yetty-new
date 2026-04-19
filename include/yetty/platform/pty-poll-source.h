#ifndef YETTY_PLATFORM_PTY_POLL_SOURCE_H
#define YETTY_PLATFORM_PTY_POLL_SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pty poll source base - opaque platform-specific pollable handle for PTY I/O
 *
 * Unix (fd_pty_poll_source): wraps the pty master fd
 * Webasm (webasm_pty_poll_source): holds ptyId + receive buffer, JS interop pushes data
 * Windows (win_pty_poll_source): wraps a HANDLE
 *
 * Platform implementations embed this as first member for structural inheritance.
 */
struct yetty_platform_pty_poll_source {
    int fd;      /* file descriptor for Unix, -1 for non-fd-based */
#ifdef _WIN32
    void *handle; /* HANDLE for Windows pipes (used with uv_pipe_t) */
#endif
};

#ifdef __cplusplus
}
#endif

#endif /* YETTY_PLATFORM_PTY_POLL_SOURCE_H */
