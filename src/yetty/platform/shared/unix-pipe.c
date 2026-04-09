/* Unix platform input pipe implementation */

#include <yetty/platform/platform-input-pipe.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* container_of macro */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Unix input pipe - embeds base as first member */
struct unix_platform_input_pipe {
    struct yetty_platform_input_pipe base;
    int read_fd;
    int write_fd;
};

/* Forward declarations */
static void unix_pipe_destroy(struct yetty_platform_input_pipe *self);
static void unix_pipe_write(struct yetty_platform_input_pipe *self, const void *data, size_t size);
static size_t unix_pipe_read(struct yetty_platform_input_pipe *self, void *data, size_t max_size);
static int unix_pipe_read_fd(const struct yetty_platform_input_pipe *self);
static void unix_pipe_set_event_loop(struct yetty_platform_input_pipe *self,
                                      struct yetty_core_event_loop *loop);

/* Ops table */
static const struct yetty_platform_input_pipe_ops unix_pipe_ops = {
    .destroy = unix_pipe_destroy,
    .write = unix_pipe_write,
    .read = unix_pipe_read,
    .read_fd = unix_pipe_read_fd,
    .set_event_loop = unix_pipe_set_event_loop,
};

/* Implementation */

static void unix_pipe_destroy(struct yetty_platform_input_pipe *self)
{
    struct unix_platform_input_pipe *pipe_impl;

    pipe_impl = container_of(self, struct unix_platform_input_pipe, base);

    if (pipe_impl->read_fd >= 0)
        close(pipe_impl->read_fd);
    if (pipe_impl->write_fd >= 0)
        close(pipe_impl->write_fd);

    free(pipe_impl);
}

static void unix_pipe_write(struct yetty_platform_input_pipe *self, const void *data, size_t size)
{
    struct unix_platform_input_pipe *pipe_impl;

    pipe_impl = container_of(self, struct unix_platform_input_pipe, base);

    if (pipe_impl->write_fd < 0 || size == 0)
        return;

    write(pipe_impl->write_fd, data, size);
}

static size_t unix_pipe_read(struct yetty_platform_input_pipe *self, void *data, size_t max_size)
{
    struct unix_platform_input_pipe *pipe_impl;
    ssize_t bytes_read;

    pipe_impl = container_of(self, struct unix_platform_input_pipe, base);

    if (pipe_impl->read_fd < 0 || max_size == 0)
        return 0;

    bytes_read = read(pipe_impl->read_fd, data, max_size);
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;  /* No data available */
        return 0;
    }

    return (size_t)bytes_read;
}

static int unix_pipe_read_fd(const struct yetty_platform_input_pipe *self)
{
    const struct unix_platform_input_pipe *pipe_impl;

    pipe_impl = container_of(self, struct unix_platform_input_pipe, base);
    return pipe_impl->read_fd;
}

static void unix_pipe_set_event_loop(struct yetty_platform_input_pipe *self,
                                      struct yetty_core_event_loop *loop)
{
    /* No-op on Unix - EventLoop polls the fd directly */
    (void)self;
    (void)loop;
}

/* Create function */

struct yetty_platform_input_pipe_result yetty_platform_input_pipe_create(void)
{
    struct unix_platform_input_pipe *pipe_impl;
    int fds[2];
    int flags;

    pipe_impl = malloc(sizeof(struct unix_platform_input_pipe));
    if (!pipe_impl)
        return YETTY_ERR(yetty_platform_input_pipe, "failed to allocate input pipe");

    pipe_impl->base.ops = &unix_pipe_ops;
    pipe_impl->read_fd = -1;
    pipe_impl->write_fd = -1;

    if (pipe(fds) != 0) {
        free(pipe_impl);
        return YETTY_ERR(yetty_platform_input_pipe, "pipe() failed");
    }

    pipe_impl->read_fd = fds[0];
    pipe_impl->write_fd = fds[1];

    /* Set read end non-blocking */
    flags = fcntl(pipe_impl->read_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(pipe_impl->read_fd, F_SETFL, flags | O_NONBLOCK);

    return YETTY_OK(yetty_platform_input_pipe, &pipe_impl->base);
}
