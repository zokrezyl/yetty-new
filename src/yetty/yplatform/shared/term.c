/* term.c - POSIX terminal helpers */

#include <yetty/yplatform/term.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

int yplatform_stderr_supports_color(void)
{
    const char *term = getenv("TERM");
    if (!term || !isatty(fileno(stderr)))
        return 0;

    return (strstr(term, "color") != NULL ||
            strstr(term, "xterm") != NULL ||
            strstr(term, "screen") != NULL ||
            strstr(term, "tmux") != NULL ||
            strcmp(term, "linux") == 0);
}

void yplatform_format_timestamp(char *buf, size_t bufsize)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm = localtime(&tv.tv_sec);
    snprintf(buf, bufsize, "%02d:%02d:%02d.%03ld",
             tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec / 1000);
}

int yplatform_stdout_write(const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static struct termios g_orig_termios;
static int g_raw_mode = 0;

static void raw_mode_disable(void)
{
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = 0;
    }
}

void yplatform_stdin_raw_mode_enable(void)
{
    if (g_raw_mode)
        return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0)
        return;
    atexit(raw_mode_disable);

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode = 1;
}

int yplatform_stdin_wait_readable(int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (r == 0) return 0;
    return (pfd.revents & POLLIN) ? 1 : 0;
}

int yplatform_stdin_read(void *buf, size_t max_len)
{
    ssize_t n = read(STDIN_FILENO, buf, max_len);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        return -1;
    }
    return (int)n;
}
