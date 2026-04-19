/* Windows ConPTY implementation */

#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/types.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* Windows PTY poll source */
struct win_pty_poll_source {
    struct yetty_platform_pty_poll_source base;
    HANDLE handle;
};

/* Windows ConPTY implementation - embeds base as first member */
struct win_conpty {
    struct yetty_platform_pty base;
    struct win_pty_poll_source poll_source;
    HPCON hpc;
    HANDLE pipe_in;
    HANDLE pipe_out;
    HANDLE process;
    uint32_t cols;
    uint32_t rows;
    int running;
};

/* Forward declarations */
static void win_conpty_destroy(struct yetty_platform_pty *self);
static struct yetty_core_size_result win_conpty_read(struct yetty_platform_pty *self, char *buf, size_t max_len);
static struct yetty_core_size_result win_conpty_write(struct yetty_platform_pty *self, const char *data, size_t len);
static struct yetty_core_void_result win_conpty_resize(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows);
static struct yetty_core_void_result win_conpty_stop(struct yetty_platform_pty *self);
static struct yetty_platform_pty_poll_source *win_conpty_poll_source(struct yetty_platform_pty *self);

/* Ops table */
static const struct yetty_platform_pty_ops win_conpty_ops = {
    .destroy = win_conpty_destroy,
    .read = win_conpty_read,
    .write = win_conpty_write,
    .resize = win_conpty_resize,
    .stop = win_conpty_stop,
    .poll_source = win_conpty_poll_source,
};

/* Windows PTY factory - embeds base as first member */
struct win_pty_factory {
    struct yetty_platform_pty_factory base;
    struct yetty_config *config;
};

/* Forward declarations for factory */
static void win_pty_factory_destroy(struct yetty_platform_pty_factory *self);
static struct yetty_platform_pty_result win_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self);

/* Factory ops table */
static const struct yetty_platform_pty_factory_ops win_pty_factory_ops = {
    .destroy = win_pty_factory_destroy,
    .create_pty = win_pty_factory_create_pty,
};

/* PTY implementation */

static void win_conpty_destroy(struct yetty_platform_pty *self)
{
    struct win_conpty *pty = container_of(self, struct win_conpty, base);

    win_conpty_stop(self);
    free(pty);
}

static struct yetty_core_size_result win_conpty_read(struct yetty_platform_pty *self, char *buf, size_t max_len)
{
    struct win_conpty *pty = container_of(self, struct win_conpty, base);
    DWORD bytes_read = 0;
    DWORD available = 0;

    if (pty->pipe_out == INVALID_HANDLE_VALUE)
        return YETTY_ERR(yetty_core_size, "conpty output pipe not open");

    /* Check if data is available (non-blocking) */
    if (!PeekNamedPipe(pty->pipe_out, NULL, 0, NULL, &available, NULL) || available == 0)
        return YETTY_OK(yetty_core_size, 0);

    if (!ReadFile(pty->pipe_out, buf, (DWORD)max_len, &bytes_read, NULL))
        return YETTY_ERR(yetty_core_size, "ReadFile from conpty failed");

    return YETTY_OK(yetty_core_size, (size_t)bytes_read);
}

static struct yetty_core_size_result win_conpty_write(struct yetty_platform_pty *self, const char *data, size_t len)
{
    struct win_conpty *pty = container_of(self, struct win_conpty, base);
    DWORD bytes_written = 0;

    if (pty->pipe_in == INVALID_HANDLE_VALUE)
        return YETTY_ERR(yetty_core_size, "conpty input pipe not open");

    if (len == 0)
        return YETTY_OK(yetty_core_size, 0);

    if (!WriteFile(pty->pipe_in, data, (DWORD)len, &bytes_written, NULL))
        return YETTY_ERR(yetty_core_size, "WriteFile to conpty failed");

    return YETTY_OK(yetty_core_size, (size_t)bytes_written);
}

static struct yetty_core_void_result win_conpty_resize(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows)
{
    struct win_conpty *pty = container_of(self, struct win_conpty, base);
    COORD size;
    HRESULT hr;

    pty->cols = cols;
    pty->rows = rows;

    if (pty->hpc == NULL)
        return YETTY_ERR(yetty_core_void, "conpty not initialized");

    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;

    hr = ResizePseudoConsole(pty->hpc, size);
    if (FAILED(hr))
        return YETTY_ERR(yetty_core_void, "ResizePseudoConsole failed");

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result win_conpty_stop(struct yetty_platform_pty *self)
{
    struct win_conpty *pty = container_of(self, struct win_conpty, base);

    if (!pty->running)
        return YETTY_OK_VOID();

    pty->running = 0;

    if (pty->process != INVALID_HANDLE_VALUE) {
        TerminateProcess(pty->process, 0);
        CloseHandle(pty->process);
        pty->process = INVALID_HANDLE_VALUE;
    }

    if (pty->hpc != NULL) {
        ClosePseudoConsole(pty->hpc);
        pty->hpc = NULL;
    }

    if (pty->pipe_in != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->pipe_in);
        pty->pipe_in = INVALID_HANDLE_VALUE;
    }

    if (pty->pipe_out != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->pipe_out);
        pty->pipe_out = INVALID_HANDLE_VALUE;
    }

    return YETTY_OK_VOID();
}

static struct yetty_platform_pty_poll_source *win_conpty_poll_source(struct yetty_platform_pty *self)
{
    struct win_conpty *pty = container_of(self, struct win_conpty, base);
    return &pty->poll_source.base;
}

/* Create ConPTY with shell */

static struct yetty_platform_pty_result win_conpty_create(struct yetty_config *config)
{
    struct win_conpty *pty;
    HANDLE pipe_pty_in = INVALID_HANDLE_VALUE;
    HANDLE pipe_pty_out = INVALID_HANDLE_VALUE;
    COORD size;
    HRESULT hr;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attr_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = NULL;
    wchar_t cmd[] = L"cmd.exe";

    (void)config;

    pty = malloc(sizeof(struct win_conpty));
    if (!pty)
        return YETTY_ERR(yetty_platform_pty, "failed to allocate conpty");

    pty->base.ops = &win_conpty_ops;
    pty->hpc = NULL;
    pty->pipe_in = INVALID_HANDLE_VALUE;
    pty->pipe_out = INVALID_HANDLE_VALUE;
    pty->process = INVALID_HANDLE_VALUE;
    pty->cols = 80;
    pty->rows = 24;
    pty->running = 0;
    pty->poll_source.base.fd = -1;
    pty->poll_source.base.handle = INVALID_HANDLE_VALUE;
    pty->poll_source.handle = INVALID_HANDLE_VALUE;

    /* Create pipes for PTY */
    if (!CreatePipe(&pipe_pty_in, &pty->pipe_in, NULL, 0)) {
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "CreatePipe for input failed");
    }

    if (!CreatePipe(&pty->pipe_out, &pipe_pty_out, NULL, 0)) {
        CloseHandle(pipe_pty_in);
        CloseHandle(pty->pipe_in);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "CreatePipe for output failed");
    }

    /* Create pseudo console */
    size.X = (SHORT)pty->cols;
    size.Y = (SHORT)pty->rows;

    hr = CreatePseudoConsole(size, pipe_pty_in, pipe_pty_out, 0, &pty->hpc);
    CloseHandle(pipe_pty_in);
    CloseHandle(pipe_pty_out);

    if (FAILED(hr)) {
        CloseHandle(pty->pipe_in);
        CloseHandle(pty->pipe_out);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "CreatePseudoConsole failed");
    }

    /* Create process with pseudo console */
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attr_list = malloc(attr_size);
    if (!attr_list) {
        ClosePseudoConsole(pty->hpc);
        CloseHandle(pty->pipe_in);
        CloseHandle(pty->pipe_out);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "failed to allocate attribute list");
    }

    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        free(attr_list);
        ClosePseudoConsole(pty->hpc);
        CloseHandle(pty->pipe_in);
        CloseHandle(pty->pipe_out);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "InitializeProcThreadAttributeList failed");
    }

    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pty->hpc, sizeof(HPCON), NULL, NULL)) {
        DeleteProcThreadAttributeList(attr_list);
        free(attr_list);
        ClosePseudoConsole(pty->hpc);
        CloseHandle(pty->pipe_in);
        CloseHandle(pty->pipe_out);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "UpdateProcThreadAttribute failed");
    }

    si.lpAttributeList = attr_list;

    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &si.StartupInfo, &pi)) {
        DeleteProcThreadAttributeList(attr_list);
        free(attr_list);
        ClosePseudoConsole(pty->hpc);
        CloseHandle(pty->pipe_in);
        CloseHandle(pty->pipe_out);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "CreateProcessW failed");
    }

    DeleteProcThreadAttributeList(attr_list);
    free(attr_list);

    CloseHandle(pi.hThread);
    pty->process = pi.hProcess;
    pty->poll_source.handle = pty->pipe_out;
    pty->poll_source.base.handle = pty->pipe_out;
    pty->running = 1;

    return YETTY_OK(yetty_platform_pty, &pty->base);
}

/* Factory implementation */

static void win_pty_factory_destroy(struct yetty_platform_pty_factory *self)
{
    struct win_pty_factory *factory = container_of(self, struct win_pty_factory, base);
    free(factory);
}

static struct yetty_platform_pty_result win_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self)
{
    struct win_pty_factory *factory = container_of(self, struct win_pty_factory, base);
    return win_conpty_create(factory->config);
}

/* Factory creation - the public API */

struct yetty_platform_pty_factory_result yetty_platform_pty_factory_create(
    struct yetty_config *config,
    void *os_specific)
{
    struct win_pty_factory *factory;

    (void)os_specific;

    factory = malloc(sizeof(struct win_pty_factory));
    if (!factory)
        return YETTY_ERR(yetty_platform_pty_factory, "failed to allocate pty factory");

    factory->base.ops = &win_pty_factory_ops;
    factory->config = config;

    return YETTY_OK(yetty_platform_pty_factory, &factory->base);
}
