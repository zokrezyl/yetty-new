/* Telnet PTY - TCP/Telnet as PTY backend
 *
 * Provides a PTY interface over TCP using telnet protocol.
 * Used for connecting to QEMU or other telnet servers.
 *
 * Implements:
 * - RFC 854  - Telnet protocol
 * - RFC 856  - Binary transmission
 * - RFC 858  - Suppress Go Ahead
 * - RFC 1073 - NAWS (window size)
 *
 * Networking, threading, and pipe I/O all go through yetty platform
 * abstractions (yetty/platform/socket.h, yetty/platform/platform-input-pipe.h,
 * yetty/yplatform/thread.h) so this file builds on Windows as well as Unix.
 */

#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/platform/socket.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/yplatform/thread.h>
#include <yetty/yplatform/time.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/types.h>
#include <yetty/ytrace.h>

#include "telnet-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Telnet protocol state machine */
enum telnet_state {
    STATE_DATA,     /* Normal data */
    STATE_IAC,      /* Received IAC */
    STATE_WILL,     /* Received IAC WILL */
    STATE_WONT,     /* Received IAC WONT */
    STATE_DO,       /* Received IAC DO */
    STATE_DONT,     /* Received IAC DONT */
    STATE_SB,       /* In subnegotiation */
    STATE_SB_IAC,   /* IAC in subnegotiation */
};

/* Telnet PTY implementation */
struct telnet_pty {
    struct yetty_yplatform_pty base;
    struct yetty_yplatform_pty_pipe_source pipe_source;

    /* Network */
    yetty_socket_fd sock;
    char *host;
    uint16_t port;

    /* Decoded-output pipe: reader thread writes, terminal polls. */
    struct yetty_yplatform_input_pipe *output_pipe;

    /* Reader thread */
    ythread_t *reader_thread;
    int running;

    /* Terminal size */
    uint32_t cols;
    uint32_t rows;

    /* Telnet state */
    enum telnet_state state;
    uint8_t subneg_buf[256];
    size_t subneg_len;

    /* Option state */
    int naws_enabled;
    int binary_enabled;
    int sga_enabled;

    /* QEMU's telnet chardev doesn't speak NAWS. Fallback: inject a
     * `stty cols X rows Y\r` to the guest shell. We wait 3s after
     * connect to give the kernel time to boot and the shell prompt to
     * appear, then send. Subsequent resizes inject immediately. */
    int stty_initial_sent;
    double connect_ts;
};

/* Forward declarations */
static void telnet_pty_destroy(struct yetty_yplatform_pty *self);
static struct yetty_ycore_size_result telnet_pty_read(struct yetty_yplatform_pty *self, char *buf, size_t max_len);
static struct yetty_ycore_size_result telnet_pty_write(struct yetty_yplatform_pty *self, const char *data, size_t len);
static struct yetty_ycore_void_result telnet_pty_resize(struct yetty_yplatform_pty *self, uint32_t cols, uint32_t rows);
static struct yetty_ycore_void_result telnet_pty_stop(struct yetty_yplatform_pty *self);
static struct yetty_yplatform_pty_pipe_source *telnet_pty_pipe_source(struct yetty_yplatform_pty *self);

/* Ops table */
static const struct yetty_yplatform_pty_ops telnet_pty_ops = {
    .destroy = telnet_pty_destroy,
    .read = telnet_pty_read,
    .write = telnet_pty_write,
    .resize = telnet_pty_resize,
    .stop = telnet_pty_stop,
    .pipe_source = telnet_pty_pipe_source,
};

/* Send raw bytes to socket. Loops past short writes; returns 0 on success. */
static int telnet_send_raw(struct telnet_pty *pty, const uint8_t *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        struct yetty_ycore_size_result r = yetty_yplatform_socket_send(
            pty->sock, data + sent, len - sent);
        if (!r.ok)
            return -1;
        if (r.value == 0) {
            /* Would block under non-blocking mode; we use blocking sockets,
             * so treat zero-length as a soft retry. */
            continue;
        }
        sent += r.value;
    }
    return 0;
}

/* Send telnet command */
static void telnet_send_cmd(struct telnet_pty *pty, uint8_t cmd, uint8_t opt)
{
    uint8_t buf[3] = { TELNET_IAC, cmd, opt };
    telnet_send_raw(pty, buf, 3);
}

/* Inject a `stty cols X rows Y\r` command into the guest shell. Used
 * when the server doesn't support NAWS (QEMU's telnet chardev). */
static void telnet_inject_stty(struct telnet_pty *pty)
{
    if (pty->cols == 0 || pty->rows == 0)
        return;
    char cmd[80];
    int n = snprintf(cmd, sizeof(cmd),
                     "\rstty cols %u rows %u\r", pty->cols, pty->rows);
    if (n > 0 && (size_t)n < sizeof(cmd))
        telnet_send_raw(pty, (const uint8_t *)cmd, (size_t)n);
    yinfo("telnet: injected stty cols %u rows %u (NAWS fallback)",
          pty->cols, pty->rows);
}

/* Send NAWS (window size) subnegotiation */
static void telnet_send_naws(struct telnet_pty *pty)
{
    if (!pty->naws_enabled)
        return;

    uint8_t buf[9];
    buf[0] = TELNET_IAC;
    buf[1] = TELNET_SB;
    buf[2] = TELOPT_NAWS;
    buf[3] = (pty->cols >> 8) & 0xff;
    buf[4] = pty->cols & 0xff;
    buf[5] = (pty->rows >> 8) & 0xff;
    buf[6] = pty->rows & 0xff;
    buf[7] = TELNET_IAC;
    buf[8] = TELNET_SE;

    telnet_send_raw(pty, buf, 9);
    ydebug("telnet: sent NAWS %ux%u", pty->cols, pty->rows);
}

/* Handle WILL option */
static void telnet_handle_will(struct telnet_pty *pty, uint8_t opt)
{
    yinfo("telnet: received WILL %u", (unsigned)opt);
    switch (opt) {
    case TELOPT_ECHO:
        telnet_send_cmd(pty, TELNET_DO, opt);
        break;
    case TELOPT_SGA:
        telnet_send_cmd(pty, TELNET_DO, opt);
        pty->sga_enabled = 1;
        break;
    case TELOPT_BINARY:
        telnet_send_cmd(pty, TELNET_DO, opt);
        pty->binary_enabled = 1;
        break;
    default:
        telnet_send_cmd(pty, TELNET_DONT, opt);
        break;
    }
}

/* Handle DO option */
static void telnet_handle_do(struct telnet_pty *pty, uint8_t opt)
{
    yinfo("telnet: received DO %u", (unsigned)opt);
    switch (opt) {
    case TELOPT_NAWS:
        telnet_send_cmd(pty, TELNET_WILL, opt);
        pty->naws_enabled = 1;
        telnet_send_naws(pty);
        break;
    case TELOPT_TTYPE:
        telnet_send_cmd(pty, TELNET_WILL, opt);
        break;
    case TELOPT_BINARY:
        telnet_send_cmd(pty, TELNET_WILL, opt);
        pty->binary_enabled = 1;
        break;
    case TELOPT_SGA:
        telnet_send_cmd(pty, TELNET_WILL, opt);
        pty->sga_enabled = 1;
        break;
    default:
        telnet_send_cmd(pty, TELNET_WONT, opt);
        break;
    }
}

/* Handle subnegotiation */
static void telnet_handle_subneg(struct telnet_pty *pty)
{
    if (pty->subneg_len < 1)
        return;

    uint8_t opt = pty->subneg_buf[0];

    if (opt == TELOPT_TTYPE && pty->subneg_len >= 2 && pty->subneg_buf[1] == TTYPE_SEND) {
        /* Send terminal type */
        const char *ttype = "xterm-256color";
        size_t tlen = strlen(ttype);
        uint8_t buf[64];
        size_t i = 0;

        buf[i++] = TELNET_IAC;
        buf[i++] = TELNET_SB;
        buf[i++] = TELOPT_TTYPE;
        buf[i++] = TTYPE_IS;
        memcpy(buf + i, ttype, tlen);
        i += tlen;
        buf[i++] = TELNET_IAC;
        buf[i++] = TELNET_SE;

        telnet_send_raw(pty, buf, i);
        ydebug("telnet: sent TTYPE %s", ttype);
    }
}

/* Append a single decoded byte to the output pipe. */
static void telnet_emit_byte(struct telnet_pty *pty, uint8_t byte)
{
    pty->output_pipe->ops->write(pty->output_pipe, &byte, 1);
}

/* Process received byte through telnet state machine */
static void telnet_process_byte(struct telnet_pty *pty, uint8_t byte)
{
    switch (pty->state) {
    case STATE_DATA:
        if (byte == TELNET_IAC) {
            pty->state = STATE_IAC;
        } else {
            telnet_emit_byte(pty, byte);
        }
        break;

    case STATE_IAC:
        switch (byte) {
        case TELNET_IAC:
            /* Escaped IAC - emit literal 255 */
            telnet_emit_byte(pty, byte);
            pty->state = STATE_DATA;
            break;
        case TELNET_WILL:
            pty->state = STATE_WILL;
            break;
        case TELNET_WONT:
            pty->state = STATE_WONT;
            break;
        case TELNET_DO:
            pty->state = STATE_DO;
            break;
        case TELNET_DONT:
            pty->state = STATE_DONT;
            break;
        case TELNET_SB:
            pty->state = STATE_SB;
            pty->subneg_len = 0;
            break;
        default:
            /* Other commands - ignore */
            pty->state = STATE_DATA;
            break;
        }
        break;

    case STATE_WILL:
        telnet_handle_will(pty, byte);
        pty->state = STATE_DATA;
        break;

    case STATE_WONT:
        /* Server won't do option - acknowledge */
        pty->state = STATE_DATA;
        break;

    case STATE_DO:
        telnet_handle_do(pty, byte);
        pty->state = STATE_DATA;
        break;

    case STATE_DONT:
        /* Server requests we don't do option - acknowledge */
        telnet_send_cmd(pty, TELNET_WONT, byte);
        pty->state = STATE_DATA;
        break;

    case STATE_SB:
        if (byte == TELNET_IAC) {
            pty->state = STATE_SB_IAC;
        } else if (pty->subneg_len < sizeof(pty->subneg_buf)) {
            pty->subneg_buf[pty->subneg_len++] = byte;
        }
        break;

    case STATE_SB_IAC:
        if (byte == TELNET_SE) {
            telnet_handle_subneg(pty);
            pty->state = STATE_DATA;
        } else if (byte == TELNET_IAC) {
            /* Escaped IAC in subnegotiation */
            if (pty->subneg_len < sizeof(pty->subneg_buf))
                pty->subneg_buf[pty->subneg_len++] = TELNET_IAC;
            pty->state = STATE_SB;
        } else {
            pty->state = STATE_DATA;
        }
        break;
    }
}

/* Reader thread - reads from socket, processes telnet, writes to pipe */
static int telnet_reader_thread(void *arg)
{
    struct telnet_pty *pty = arg;
    uint8_t buf[4096];

    yinfo("telnet_reader: started");

    /* Non-blocking + 100ms sleep so we can periodically check whether to
     * fire the one-shot stty injection (NAWS fallback for QEMU which
     * doesn't negotiate NAWS). */
    yetty_yplatform_socket_set_nonblocking(pty->sock);

    while (pty->running) {
        if (!pty->stty_initial_sent && !pty->naws_enabled &&
            pty->cols > 0 && pty->rows > 0) {
            double elapsed = ytime_monotonic_sec() - pty->connect_ts;
            if (elapsed >= 3.0) {
                telnet_inject_stty(pty);
                pty->stty_initial_sent = 1;
            }
        }

        struct yetty_ycore_size_result r = yetty_yplatform_socket_recv(
            pty->sock, buf, sizeof(buf));
        if (!r.ok) {
            yinfo("telnet_reader: recv error, exiting");
            break;
        }
        if (r.value == 0) {
            /* Could be EOF or would-block on the non-blocking socket. */
            if (yetty_yplatform_socket_would_block()) {
                ytime_sleep_ms(100);
                continue;
            }
            yinfo("telnet_reader: connection closed");
            break;
        }
        for (size_t i = 0; i < r.value; i++)
            telnet_process_byte(pty, buf[i]);
    }

    yinfo("telnet_reader: exiting");
    return 0;
}

/* Connect to telnet server */
static yetty_socket_fd telnet_connect(struct telnet_pty *pty)
{
    /* Idempotent — wraps WSAStartup on Windows, no-op elsewhere. */
    if (!yetty_yplatform_socket_init()) {
        yerror("telnet: socket subsystem init failed");
        return YETTY_SOCKET_INVALID;
    }

    struct yetty_socket_fd_result fd_r = yetty_yplatform_socket_create_tcp();
    if (!fd_r.ok) {
        yerror("telnet: socket create failed");
        return YETTY_SOCKET_INVALID;
    }

    /* Blocking connect — sock is left in blocking mode for the dedicated
     * reader thread. */
    struct yetty_ycore_void_result cr = yetty_yplatform_socket_connect(
        fd_r.value, pty->host, pty->port);
    if (!cr.ok) {
        yerror("telnet: failed to connect to %s:%u", pty->host, pty->port);
        yetty_yplatform_socket_close(fd_r.value);
        return YETTY_SOCKET_INVALID;
    }

    /* Best-effort TCP_NODELAY for low latency. */
    yetty_yplatform_socket_set_nodelay(fd_r.value, 1);

    yinfo("telnet: connected to %s:%u", pty->host, pty->port);
    return fd_r.value;
}

/* PTY implementation */

static void telnet_pty_destroy(struct yetty_yplatform_pty *self)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    telnet_pty_stop(self);

    if (pty->output_pipe) {
        pty->output_pipe->ops->destroy(pty->output_pipe);
        pty->output_pipe = NULL;
    }

    free(pty->host);
    free(pty);
}

static struct yetty_ycore_size_result telnet_pty_read(struct yetty_yplatform_pty *self, char *buf, size_t max_len)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    if (!pty->running || max_len == 0)
        return YETTY_OK(yetty_ycore_size, 0);

    return pty->output_pipe->ops->read(pty->output_pipe, buf, max_len);
}

static struct yetty_ycore_size_result telnet_pty_write(struct yetty_yplatform_pty *self, const char *data, size_t len)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    if (!pty->running || len == 0)
        return YETTY_OK(yetty_ycore_size, 0);

    /* Escape IAC bytes in data */
    uint8_t buf[4096];
    size_t j = 0;

    for (size_t i = 0; i < len && j < sizeof(buf) - 1; i++) {
        uint8_t c = (uint8_t)data[i];
        if (c == TELNET_IAC) {
            buf[j++] = TELNET_IAC;
            buf[j++] = TELNET_IAC;
        } else {
            buf[j++] = c;
        }
    }

    if (telnet_send_raw(pty, buf, j) < 0)
        return YETTY_OK(yetty_ycore_size, 0);

    return YETTY_OK(yetty_ycore_size, len);
}

static struct yetty_ycore_void_result telnet_pty_resize(struct yetty_yplatform_pty *self, uint32_t cols, uint32_t rows)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    pty->cols = cols;
    pty->rows = rows;

    if (!pty->running)
        return YETTY_OK_VOID();

    if (pty->naws_enabled) {
        telnet_send_naws(pty);
    } else if (pty->stty_initial_sent) {
        /* Subsequent resizes after the initial inject — send right away. */
        telnet_inject_stty(pty);
    }
    /* If !stty_initial_sent, the reader thread will handle the first
     * inject once the boot grace period elapses. */

    return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result telnet_pty_stop(struct yetty_yplatform_pty *self)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    if (!pty->running)
        return YETTY_OK_VOID();

    pty->running = 0;

    if (pty->sock != YETTY_SOCKET_INVALID) {
        /* Closing the socket will unblock the reader thread's recv. */
        yetty_yplatform_socket_close(pty->sock);
        pty->sock = YETTY_SOCKET_INVALID;
    }

    if (pty->reader_thread) {
        ythread_join(pty->reader_thread);
        pty->reader_thread = NULL;
    }

    return YETTY_OK_VOID();
}

static struct yetty_yplatform_pty_pipe_source *telnet_pty_pipe_source(struct yetty_yplatform_pty *self)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;
    return &pty->pipe_source;
}

/* Create telnet PTY */
struct yetty_yplatform_pty_result telnet_pty_create(const char *host, uint16_t port)
{
    struct telnet_pty *pty;

    pty = calloc(1, sizeof(struct telnet_pty));
    if (!pty)
        return YETTY_ERR(yetty_yplatform_pty, "failed to allocate telnet pty");

    pty->base.ops = &telnet_pty_ops;
    pty->sock = YETTY_SOCKET_INVALID;
    pty->cols = 80;
    pty->rows = 24;
    pty->state = STATE_DATA;

    pty->host = strdup(host);
    pty->port = port;

    if (!pty->host) {
        free(pty);
        return YETTY_ERR(yetty_yplatform_pty, "failed to allocate host string");
    }

    /* Decoded-output pipe (read end exposed to event loop). */
    struct yetty_yplatform_input_pipe_result pr = yetty_yplatform_input_pipe_create();
    if (!pr.ok) {
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_yplatform_pty, "failed to create output pipe");
    }
    pty->output_pipe = pr.value;

    struct yetty_ycore_int_result fdr = pty->output_pipe->ops->read_fd(pty->output_pipe);
    if (!fdr.ok) {
        pty->output_pipe->ops->destroy(pty->output_pipe);
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_yplatform_pty, "failed to obtain pipe read fd");
    }
    pty->pipe_source.abstract = (uintptr_t)fdr.value;

    /* Connect */
    pty->sock = telnet_connect(pty);
    if (pty->sock == YETTY_SOCKET_INVALID) {
        pty->output_pipe->ops->destroy(pty->output_pipe);
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_yplatform_pty, "failed to connect");
    }

    /* Proactively offer WILL NAWS. Real telnet servers (BSD telnetd, etc.)
     * reply DO NAWS and we then send proper window-size subnegotiations.
     * QEMU's telnet chardev does NOT respond — naws_enabled stays 0,
     * and the reader thread falls back to injecting a `stty cols X rows Y`
     * 3 seconds after connect. */
    telnet_send_cmd(pty, TELNET_WILL, TELOPT_NAWS);
    pty->connect_ts = ytime_monotonic_sec();

    /* Start reader thread */
    pty->running = 1;
    pty->reader_thread = ythread_create(telnet_reader_thread, pty);
    if (!pty->reader_thread) {
        yetty_yplatform_socket_close(pty->sock);
        pty->output_pipe->ops->destroy(pty->output_pipe);
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_yplatform_pty, "failed to create reader thread");
    }

    return YETTY_OK(yetty_yplatform_pty, &pty->base);
}

/* Factory implementation */

struct telnet_pty_factory {
    struct yetty_yplatform_pty_factory base;
    char *host;
    uint16_t port;
};

static void telnet_pty_factory_destroy(struct yetty_yplatform_pty_factory *self)
{
    struct telnet_pty_factory *factory = (struct telnet_pty_factory *)self;
    free(factory->host);
    free(factory);
}

static struct yetty_yplatform_pty_result telnet_pty_factory_create_pty(
    struct yetty_yplatform_pty_factory *self)
{
    struct telnet_pty_factory *factory = (struct telnet_pty_factory *)self;
    return telnet_pty_create(factory->host, factory->port);
}

static const struct yetty_yplatform_pty_factory_ops telnet_pty_factory_ops = {
    .destroy = telnet_pty_factory_destroy,
    .create_pty = telnet_pty_factory_create_pty,
};

struct yetty_yplatform_pty_factory_result telnet_pty_factory_create(
    const char *host, uint16_t port)
{
    struct telnet_pty_factory *factory;

    factory = calloc(1, sizeof(struct telnet_pty_factory));
    if (!factory)
        return YETTY_ERR(yetty_yplatform_pty_factory, "failed to allocate telnet pty factory");

    factory->base.ops = &telnet_pty_factory_ops;
    factory->host = strdup(host);
    factory->port = port;

    if (!factory->host) {
        free(factory);
        return YETTY_ERR(yetty_yplatform_pty_factory, "failed to allocate host string");
    }

    return YETTY_OK(yetty_yplatform_pty_factory, &factory->base);
}
