/* term.c - Windows terminal helpers */

#include <yetty/yplatform/term.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int yplatform_stderr_supports_color(void)
{
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (hErr != INVALID_HANDLE_VALUE && GetConsoleMode(hErr, &mode)) {
        SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        return 1;
    }
    return 0;
}

void yplatform_format_timestamp(char *buf, size_t bufsize)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, bufsize, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

int yplatform_stdout_write(const void *data, size_t len)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;

    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        DWORD chunk = (len - off) > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)(len - off);
        DWORD written = 0;
        if (!WriteFile(h, p + off, chunk, &written, NULL))
            return -1;
        if (written == 0) return -1;  /* pipe closed */
        off += written;
    }
    return 0;
}

static HANDLE g_stdin_handle = INVALID_HANDLE_VALUE;
static DWORD  g_orig_console_mode = 0;
static int    g_raw_mode = 0;
static int    g_stdin_is_console = 0;

static void raw_mode_disable(void)
{
    if (g_raw_mode && g_stdin_is_console &&
        g_stdin_handle != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_stdin_handle, g_orig_console_mode);
        g_raw_mode = 0;
    }
}

void yplatform_stdin_raw_mode_enable(void)
{
    if (g_raw_mode)
        return;
    g_stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (g_stdin_handle == INVALID_HANDLE_VALUE)
        return;

    /* Only meaningful on a real console — pipes/files have no mode flags. */
    DWORD mode = 0;
    if (!GetConsoleMode(g_stdin_handle, &mode)) {
        g_stdin_is_console = 0;
        g_raw_mode = 1;  /* mark "configured" so we don't retry */
        return;
    }
    g_stdin_is_console = 1;
    g_orig_console_mode = mode;
    atexit(raw_mode_disable);

    /* Disable line buffering, echo, and ctrl-c/ctrl-break processing.
     * Enable virtual terminal input so escape sequences arrive verbatim. */
    DWORD raw = mode;
    raw &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    raw |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(g_stdin_handle, raw);
    g_raw_mode = 1;
}

int yplatform_stdin_wait_readable(int timeout_ms)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;

    DWORD r = WaitForSingleObject(h, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (r == WAIT_OBJECT_0) return 1;
    if (r == WAIT_TIMEOUT)  return 0;
    return -1;
}

int yplatform_stdin_read(void *buf, size_t max_len)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;

    DWORD chunk = max_len > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)max_len;
    DWORD got = 0;
    if (!ReadFile(h, buf, chunk, &got, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return (int)got;
}
