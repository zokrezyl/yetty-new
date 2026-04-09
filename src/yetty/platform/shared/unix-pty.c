/* Unix PTY I/O using forkpty() - shared by Linux and macOS */

#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

/* container_of macro */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Unix PTY implementation - embeds base as first member */
struct unix_pty {
    struct yetty_platform_pty base;
    struct yetty_platform_pty_poll_source poll_source;
    int pty_master;
    pid_t child_pid;
    uint32_t cols;
    uint32_t rows;
    int running;
};

/* Forward declarations */
static void unix_pty_destroy(struct yetty_platform_pty *self);
static size_t unix_pty_read(struct yetty_platform_pty *self, char *buf, size_t max_len);
static void unix_pty_write(struct yetty_platform_pty *self, const char *data, size_t len);
static void unix_pty_resize(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows);
static void unix_pty_stop(struct yetty_platform_pty *self);
static struct yetty_platform_pty_poll_source *unix_pty_poll_source(struct yetty_platform_pty *self);

/* Ops table */
static const struct yetty_platform_pty_ops unix_pty_ops = {
    .destroy = unix_pty_destroy,
    .read = unix_pty_read,
    .write = unix_pty_write,
    .resize = unix_pty_resize,
    .stop = unix_pty_stop,
    .poll_source = unix_pty_poll_source,
};

/* Unix PTY factory - embeds base as first member */
struct unix_pty_factory {
    struct yetty_platform_pty_factory base;
    struct yetty_config *config;
};

/* Forward declarations for factory */
static void unix_pty_factory_destroy(struct yetty_platform_pty_factory *self);
static struct yetty_platform_pty_result unix_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self);

/* Factory ops table */
static const struct yetty_platform_pty_factory_ops unix_pty_factory_ops = {
    .destroy = unix_pty_factory_destroy,
    .create_pty = unix_pty_factory_create_pty,
};

/* PTY implementation */

static void unix_pty_destroy(struct yetty_platform_pty *self)
{
    struct unix_pty *pty = container_of(self, struct unix_pty, base);

    unix_pty_stop(self);
    free(pty);
}

static size_t unix_pty_read(struct yetty_platform_pty *self, char *buf, size_t max_len)
{
    struct unix_pty *pty = container_of(self, struct unix_pty, base);
    ssize_t n;

    if (pty->pty_master < 0)
        return 0;

    n = read(pty->pty_master, buf, max_len);
    if (n > 0)
        return (size_t)n;

    return 0;
}

static struct yetty_core_void_result unix_pty_write(struct yetty_platform_pty *self, const char *data, size_t len)
{
    struct unix_pty *pty = container_of(self, struct unix_pty, base);
    ssize_t written;

    if (pty->pty_master < 0)
        return YETTY_ERR(yetty_core_void, "pty master not open");

    if (len == 0)
        return YETTY_OK_VOID();

    written = write(pty->pty_master, data, len);
    if (written < 0)
        return YETTY_ERR(yetty_core_void, "write to pty failed");

    if ((size_t)written != len)
        return YETTY_ERR(yetty_core_void, "partial write to pty");

    return YETTY_OK_VOID();
}

static void unix_pty_resize(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows)
{
    struct unix_pty *pty = container_of(self, struct unix_pty, base);
    struct winsize ws;

    pty->cols = cols;
    pty->rows = rows;

    if (pty->pty_master >= 0) {
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(pty->pty_master, TIOCSWINSZ, &ws);
    }
}

static void unix_pty_stop(struct yetty_platform_pty *self)
{
    struct unix_pty *pty = container_of(self, struct unix_pty, base);
    int status;

    if (!pty->running)
        return;

    pty->running = 0;

    if (pty->pty_master >= 0) {
        close(pty->pty_master);
        pty->pty_master = -1;
    }

    if (pty->child_pid > 0) {
        kill(pty->child_pid, SIGTERM);
        waitpid(pty->child_pid, &status, 0);
        pty->child_pid = -1;
    }
}

static struct yetty_platform_pty_poll_source *unix_pty_poll_source(struct yetty_platform_pty *self)
{
    struct unix_pty *pty = container_of(self, struct unix_pty, base);
    return &pty->poll_source;
}

/* Create PTY with shell */

static struct yetty_platform_pty_result unix_pty_create(struct yetty_config *config)
{
    struct unix_pty *pty;
    const char *shell;
    struct winsize ws;
    int flags;

    pty = malloc(sizeof(struct unix_pty));
    if (!pty)
        return YETTY_ERR(yetty_platform_pty, "failed to allocate pty");

    pty->base.ops = &unix_pty_ops;
    pty->pty_master = -1;
    pty->child_pid = -1;
    pty->cols = 80;
    pty->rows = 24;
    pty->running = 0;
    pty->poll_source.fd = -1;

    shell = config->ops->get_string(config, "shell/path", "/bin/bash");

    ws.ws_row = (unsigned short)pty->rows;
    ws.ws_col = (unsigned short)pty->cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    pty->child_pid = forkpty(&pty->pty_master, NULL, NULL, &ws);

    if (pty->child_pid < 0) {
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "forkpty failed");
    }

    if (pty->child_pid == 0) {
        /* Child process */
        int fd;
        for (fd = 3; fd < 1024; fd++)
            close(fd);

        execl(shell, shell, NULL);
        _exit(1);
    }

    /* Parent process - set non-blocking */
    flags = fcntl(pty->pty_master, F_GETFL, 0);
    fcntl(pty->pty_master, F_SETFL, flags | O_NONBLOCK);

    pty->poll_source.fd = pty->pty_master;
    pty->running = 1;

    return YETTY_OK(yetty_platform_pty, &pty->base);
}

/* Factory implementation */

static void unix_pty_factory_destroy(struct yetty_platform_pty_factory *self)
{
    struct unix_pty_factory *factory = container_of(self, struct unix_pty_factory, base);
    free(factory);
}

static struct yetty_platform_pty_result unix_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self)
{
    struct unix_pty_factory *factory = container_of(self, struct unix_pty_factory, base);
    return unix_pty_create(factory->config);
}

/* Factory creation - the public API */

struct yetty_platform_pty_factory_result yetty_platform_pty_factory_create(
    struct yetty_config *config,
    void *os_specific)
{
    struct unix_pty_factory *factory;

    (void)os_specific;

    factory = malloc(sizeof(struct unix_pty_factory));
    if (!factory)
        return YETTY_ERR(yetty_platform_pty_factory, "failed to allocate pty factory");

    factory->base.ops = &unix_pty_factory_ops;
    factory->config = config;

    return YETTY_OK(yetty_platform_pty_factory, &factory->base);
}
