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
 */

#include <yetty/platform/pty.h>
#include <yetty/platform/pty-factory.h>
#include <yetty/yconfig.h>
#include <yetty/ycore/types.h>
#include <yetty/ytrace.h>

#include "telnet-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    struct yetty_platform_pty base;
    struct yetty_platform_pty_pipe_source pipe_source;

    /* Network */
    int socket;
    char *host;
    uint16_t port;

    /* Pipe for event loop integration: reader thread writes, terminal polls */
    int output_pipe[2];

    /* Reader thread */
    pthread_t reader_thread;
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
};

/* Forward declarations */
static void telnet_pty_destroy(struct yetty_platform_pty *self);
static struct yetty_core_size_result telnet_pty_read(struct yetty_platform_pty *self, char *buf, size_t max_len);
static struct yetty_core_size_result telnet_pty_write(struct yetty_platform_pty *self, const char *data, size_t len);
static struct yetty_core_void_result telnet_pty_resize(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows);
static struct yetty_core_void_result telnet_pty_stop(struct yetty_platform_pty *self);
static struct yetty_platform_pty_pipe_source *telnet_pty_pipe_source(struct yetty_platform_pty *self);

/* Ops table */
static const struct yetty_platform_pty_ops telnet_pty_ops = {
    .destroy = telnet_pty_destroy,
    .read = telnet_pty_read,
    .write = telnet_pty_write,
    .resize = telnet_pty_resize,
    .stop = telnet_pty_stop,
    .pipe_source = telnet_pty_pipe_source,
};

/* Send raw bytes to socket */
static int telnet_send_raw(struct telnet_pty *pty, const uint8_t *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(pty->socket, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

/* Send telnet command */
static void telnet_send_cmd(struct telnet_pty *pty, uint8_t cmd, uint8_t opt)
{
    uint8_t buf[3] = { TELNET_IAC, cmd, opt };
    telnet_send_raw(pty, buf, 3);
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

/* Process received byte through telnet state machine */
static void telnet_process_byte(struct telnet_pty *pty, uint8_t byte, int *out_pipe)
{
    switch (pty->state) {
    case STATE_DATA:
        if (byte == TELNET_IAC) {
            pty->state = STATE_IAC;
        } else {
            /* Write data byte to output pipe */
            write(*out_pipe, &byte, 1);
        }
        break;

    case STATE_IAC:
        switch (byte) {
        case TELNET_IAC:
            /* Escaped IAC - write literal 255 */
            write(*out_pipe, &byte, 1);
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
static void *telnet_reader_thread(void *arg)
{
    struct telnet_pty *pty = arg;
    uint8_t buf[4096];

    yinfo("telnet_reader: started");

    while (pty->running) {
        ssize_t n = recv(pty->socket, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            yinfo("telnet_reader: connection closed");
            break;
        }

        for (ssize_t i = 0; i < n; i++) {
            telnet_process_byte(pty, buf[i], &pty->output_pipe[1]);
        }
    }

    yinfo("telnet_reader: exiting");
    return NULL;
}

/* Connect to telnet server */
static int telnet_connect(struct telnet_pty *pty)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    int sock = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", pty->port);

    int err = getaddrinfo(pty->host, port_str, &hints, &res);
    if (err != 0) {
        yerror("telnet: getaddrinfo failed: %s", gai_strerror(err));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0)
            continue;

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);

    if (sock < 0) {
        yerror("telnet: failed to connect to %s:%u", pty->host, pty->port);
        return -1;
    }

    /* Set TCP_NODELAY for low latency */
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    yinfo("telnet: connected to %s:%u", pty->host, pty->port);
    return sock;
}

/* PTY implementation */

static void telnet_pty_destroy(struct yetty_platform_pty *self)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    telnet_pty_stop(self);

    if (pty->output_pipe[0] >= 0) close(pty->output_pipe[0]);
    if (pty->output_pipe[1] >= 0) close(pty->output_pipe[1]);

    free(pty->host);
    free(pty);
}

static struct yetty_core_size_result telnet_pty_read(struct yetty_platform_pty *self, char *buf, size_t max_len)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    if (!pty->running || max_len == 0)
        return YETTY_OK(yetty_core_size, 0);

    ssize_t n = read(pty->output_pipe[0], buf, max_len);
    if (n < 0)
        n = 0;

    return YETTY_OK(yetty_core_size, (size_t)n);
}

static struct yetty_core_size_result telnet_pty_write(struct yetty_platform_pty *self, const char *data, size_t len)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    if (!pty->running || len == 0)
        return YETTY_OK(yetty_core_size, 0);

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
        return YETTY_OK(yetty_core_size, 0);

    return YETTY_OK(yetty_core_size, len);
}

static struct yetty_core_void_result telnet_pty_resize(struct yetty_platform_pty *self, uint32_t cols, uint32_t rows)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    pty->cols = cols;
    pty->rows = rows;

    if (pty->running && pty->naws_enabled)
        telnet_send_naws(pty);

    return YETTY_OK_VOID();
}

static struct yetty_core_void_result telnet_pty_stop(struct yetty_platform_pty *self)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;

    if (!pty->running)
        return YETTY_OK_VOID();

    pty->running = 0;

    if (pty->socket >= 0) {
        shutdown(pty->socket, SHUT_RDWR);
        close(pty->socket);
        pty->socket = -1;
    }

    if (pty->reader_thread) {
        pthread_join(pty->reader_thread, NULL);
        pty->reader_thread = 0;
    }

    return YETTY_OK_VOID();
}

static struct yetty_platform_pty_pipe_source *telnet_pty_pipe_source(struct yetty_platform_pty *self)
{
    struct telnet_pty *pty = (struct telnet_pty *)self;
    return &pty->pipe_source;
}

/* Create telnet PTY */
struct yetty_platform_pty_result telnet_pty_create(const char *host, uint16_t port)
{
    struct telnet_pty *pty;

    pty = calloc(1, sizeof(struct telnet_pty));
    if (!pty)
        return YETTY_ERR(yetty_platform_pty, "failed to allocate telnet pty");

    pty->base.ops = &telnet_pty_ops;
    pty->socket = -1;
    pty->output_pipe[0] = -1;
    pty->output_pipe[1] = -1;
    pty->cols = 80;
    pty->rows = 24;
    pty->state = STATE_DATA;

    pty->host = strdup(host);
    pty->port = port;

    if (!pty->host) {
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "failed to allocate host string");
    }

    /* Create output pipe */
    if (pipe(pty->output_pipe) < 0) {
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "failed to create output pipe");
    }

    fcntl(pty->output_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(pty->output_pipe[1], F_SETFL, O_NONBLOCK);

    pty->pipe_source.abstract = pty->output_pipe[0];

    /* Connect */
    pty->socket = telnet_connect(pty);
    if (pty->socket < 0) {
        close(pty->output_pipe[0]);
        close(pty->output_pipe[1]);
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "failed to connect");
    }

    /* Start reader thread */
    pty->running = 1;
    if (pthread_create(&pty->reader_thread, NULL, telnet_reader_thread, pty) != 0) {
        close(pty->socket);
        close(pty->output_pipe[0]);
        close(pty->output_pipe[1]);
        free(pty->host);
        free(pty);
        return YETTY_ERR(yetty_platform_pty, "failed to create reader thread");
    }

    return YETTY_OK(yetty_platform_pty, &pty->base);
}

/* Factory implementation */

struct telnet_pty_factory {
    struct yetty_platform_pty_factory base;
    char *host;
    uint16_t port;
};

static void telnet_pty_factory_destroy(struct yetty_platform_pty_factory *self)
{
    struct telnet_pty_factory *factory = (struct telnet_pty_factory *)self;
    free(factory->host);
    free(factory);
}

static struct yetty_platform_pty_result telnet_pty_factory_create_pty(
    struct yetty_platform_pty_factory *self)
{
    struct telnet_pty_factory *factory = (struct telnet_pty_factory *)self;
    return telnet_pty_create(factory->host, factory->port);
}

static const struct yetty_platform_pty_factory_ops telnet_pty_factory_ops = {
    .destroy = telnet_pty_factory_destroy,
    .create_pty = telnet_pty_factory_create_pty,
};

struct yetty_platform_pty_factory_result telnet_pty_factory_create(
    const char *host, uint16_t port)
{
    struct telnet_pty_factory *factory;

    factory = calloc(1, sizeof(struct telnet_pty_factory));
    if (!factory)
        return YETTY_ERR(yetty_platform_pty_factory, "failed to allocate telnet pty factory");

    factory->base.ops = &telnet_pty_factory_ops;
    factory->host = strdup(host);
    factory->port = port;

    if (!factory->host) {
        free(factory);
        return YETTY_ERR(yetty_platform_pty_factory, "failed to allocate host string");
    }

    return YETTY_OK(yetty_platform_pty_factory, &factory->base);
}
