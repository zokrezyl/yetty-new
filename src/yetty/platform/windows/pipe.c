/* Windows platform input pipe implementation */

#include <yetty/platform/platform-input-pipe.h>
#include <yetty/core/types.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Windows input pipe - embeds base as first member */
struct win_platform_input_pipe {
    struct yetty_platform_input_pipe base;
    HANDLE read_handle;
    HANDLE write_handle;
};

/* Forward declarations */
static void win_pipe_destroy(struct yetty_platform_input_pipe *self);
static struct yetty_core_size_result win_pipe_write(struct yetty_platform_input_pipe *self, const void *data, size_t size);
static struct yetty_core_size_result win_pipe_read(struct yetty_platform_input_pipe *self, void *data, size_t max_size);
static struct yetty_core_int_result win_pipe_read_fd(const struct yetty_platform_input_pipe *self);
static struct yetty_core_void_result win_pipe_set_event_loop(struct yetty_platform_input_pipe *self,
                                                              struct yetty_core_event_loop *loop);

/* Ops table */
static const struct yetty_platform_input_pipe_ops win_pipe_ops = {
    .destroy = win_pipe_destroy,
    .write = win_pipe_write,
    .read = win_pipe_read,
    .read_fd = win_pipe_read_fd,
    .set_event_loop = win_pipe_set_event_loop,
};

/* Implementation */

static void win_pipe_destroy(struct yetty_platform_input_pipe *self)
{
    struct win_platform_input_pipe *pipe_impl;

    pipe_impl = container_of(self, struct win_platform_input_pipe, base);

    if (pipe_impl->read_handle != INVALID_HANDLE_VALUE)
        CloseHandle(pipe_impl->read_handle);
    if (pipe_impl->write_handle != INVALID_HANDLE_VALUE)
        CloseHandle(pipe_impl->write_handle);

    free(pipe_impl);
}

static struct yetty_core_size_result win_pipe_write(struct yetty_platform_input_pipe *self, const void *data, size_t size)
{
    struct win_platform_input_pipe *pipe_impl = container_of(self, struct win_platform_input_pipe, base);
    DWORD bytes_written = 0;

    if (pipe_impl->write_handle == INVALID_HANDLE_VALUE)
        return YETTY_ERR(yetty_core_size, "pipe write handle not open");

    if (size == 0)
        return YETTY_OK(yetty_core_size, 0);

    if (!WriteFile(pipe_impl->write_handle, data, (DWORD)size, &bytes_written, NULL))
        return YETTY_ERR(yetty_core_size, "WriteFile to pipe failed");

    return YETTY_OK(yetty_core_size, (size_t)bytes_written);
}

static struct yetty_core_size_result win_pipe_read(struct yetty_platform_input_pipe *self, void *data, size_t max_size)
{
    struct win_platform_input_pipe *pipe_impl;
    DWORD bytes_read = 0;
    DWORD available = 0;

    pipe_impl = container_of(self, struct win_platform_input_pipe, base);

    if (pipe_impl->read_handle == INVALID_HANDLE_VALUE)
        return YETTY_ERR(yetty_core_size, "pipe read handle not open");

    if (max_size == 0)
        return YETTY_OK(yetty_core_size, 0);

    /* Check if data is available (non-blocking) */
    if (!PeekNamedPipe(pipe_impl->read_handle, NULL, 0, NULL, &available, NULL) || available == 0)
        return YETTY_OK(yetty_core_size, 0);

    if (!ReadFile(pipe_impl->read_handle, data, (DWORD)max_size, &bytes_read, NULL))
        return YETTY_ERR(yetty_core_size, "ReadFile from pipe failed");

    return YETTY_OK(yetty_core_size, (size_t)bytes_read);
}

static struct yetty_core_int_result win_pipe_read_fd(const struct yetty_platform_input_pipe *self)
{
    (void)self;
    /* Windows doesn't use file descriptors for handles */
    return YETTY_ERR(yetty_core_int, "Windows pipes don't have file descriptors");
}

static struct yetty_core_void_result win_pipe_set_event_loop(struct yetty_platform_input_pipe *self,
                                                              struct yetty_core_event_loop *loop)
{
    /* TODO: integrate with Windows event loop when needed */
    (void)self;
    (void)loop;
    return YETTY_OK_VOID();
}

/* Create function */

struct yetty_platform_input_pipe_result yetty_platform_input_pipe_create(void)
{
    struct win_platform_input_pipe *pipe_impl;
    SECURITY_ATTRIBUTES sa;

    pipe_impl = malloc(sizeof(struct win_platform_input_pipe));
    if (!pipe_impl)
        return YETTY_ERR(yetty_platform_input_pipe, "failed to allocate input pipe");

    pipe_impl->base.ops = &win_pipe_ops;
    pipe_impl->read_handle = INVALID_HANDLE_VALUE;
    pipe_impl->write_handle = INVALID_HANDLE_VALUE;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    if (!CreatePipe(&pipe_impl->read_handle, &pipe_impl->write_handle, &sa, 0)) {
        free(pipe_impl);
        return YETTY_ERR(yetty_platform_input_pipe, "CreatePipe failed");
    }

    return YETTY_OK(yetty_platform_input_pipe, &pipe_impl->base);
}
