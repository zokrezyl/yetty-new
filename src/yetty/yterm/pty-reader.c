/* pty-reader.c - PTY reader with OSC parsing and dispatch */

#include <yetty/yterm/pty-reader.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>

#define PTY_READ_BUF_SIZE 4096
#define PTY_READ_MAX_CHUNK 65536
#define OSC_BUF_INITIAL 4096
#define OSC_BUF_MAX (64 * 1024 * 1024)
#define MAX_OSC_SINKS 64

enum osc_state {
    OSC_STATE_NORMAL,
    OSC_STATE_ESC,
    OSC_STATE_IN_OSC,
    OSC_STATE_OSC_ESC_END
};

struct osc_sink {
    int vendor_id;
    struct yetty_term_terminal_layer *layer;
};

struct yetty_term_pty_reader {
    struct yetty_platform_pty *pty;
    struct yetty_term_terminal_layer *default_sink;
    struct osc_sink osc_sinks[MAX_OSC_SINKS];
    size_t osc_sink_count;
    enum osc_state state;
    char *osc_buf;
    size_t osc_buf_len;
    size_t osc_buf_cap;
};

static int osc_buf_append(struct yetty_term_pty_reader *r, char c)
{
    if (r->osc_buf_len >= OSC_BUF_MAX)
        return 0;

    if (r->osc_buf_len >= r->osc_buf_cap) {
        size_t new_cap = r->osc_buf_cap == 0 ? OSC_BUF_INITIAL : r->osc_buf_cap * 2;
        if (new_cap > OSC_BUF_MAX)
            new_cap = OSC_BUF_MAX;
        char *new_buf = realloc(r->osc_buf, new_cap);
        if (!new_buf)
            return 0;
        r->osc_buf = new_buf;
        r->osc_buf_cap = new_cap;
    }
    r->osc_buf[r->osc_buf_len++] = c;
    return 1;
}

static struct yetty_term_terminal_layer *find_osc_sink(
    struct yetty_term_pty_reader *r, int vendor_id)
{
    for (size_t i = 0; i < r->osc_sink_count; i++) {
        if (r->osc_sinks[i].vendor_id == vendor_id)
            return r->osc_sinks[i].layer;
    }
    return NULL;
}

static void dispatch_osc(struct yetty_term_pty_reader *r)
{
    if (r->osc_buf_len == 0)
        return;

    /* Parse vendor ID */
    const char *semi = memchr(r->osc_buf, ';', r->osc_buf_len);
    if (!semi) {
        ydebug("pty_reader: OSC without semicolon, len=%zu", r->osc_buf_len);
        return;
    }

    size_t id_len = semi - r->osc_buf;
    if (id_len == 0 || id_len > 10)
        return;

    char id_str[16];
    memcpy(id_str, r->osc_buf, id_len);
    id_str[id_len] = '\0';

    char *endptr;
    long vendor_id = strtol(id_str, &endptr, 10);
    if (*endptr != '\0')
        return;

    /* Find sink and dispatch payload (after semicolon) */
    struct yetty_term_terminal_layer *layer = find_osc_sink(r, (int)vendor_id);
    if (layer && layer->ops && layer->ops->write) {
        const char *payload = semi + 1;
        size_t payload_len = r->osc_buf_len - id_len - 1;
        ydebug("pty_reader: OSC %ld -> layer %p, payload_len=%zu",
               vendor_id, (void *)layer, payload_len);
        layer->ops->write(layer, payload, payload_len);
    } else {
        ydebug("pty_reader: no sink for OSC %ld", vendor_id);
    }
}

static void process_data(struct yetty_term_pty_reader *r, const char *data, size_t len)
{
    size_t i = 0;
    size_t normal_start = 0;

    while (i < len) {
        char c = data[i];

        switch (r->state) {
        case OSC_STATE_NORMAL:
            if (c == '\033') {
                /* Flush normal data before ESC */
                if (i > normal_start && r->default_sink &&
                    r->default_sink->ops && r->default_sink->ops->write) {
                    r->default_sink->ops->write(r->default_sink,
                        data + normal_start, i - normal_start);
                }
                r->state = OSC_STATE_ESC;
                i++;
            } else {
                i++;
            }
            break;

        case OSC_STATE_ESC:
            if (c == ']') {
                r->state = OSC_STATE_IN_OSC;
                r->osc_buf_len = 0;
                normal_start = i + 1;
                i++;
            } else {
                /* Not OSC, send ESC + this char as normal */
                r->state = OSC_STATE_NORMAL;
                if (r->default_sink && r->default_sink->ops && r->default_sink->ops->write) {
                    r->default_sink->ops->write(r->default_sink, "\033", 1);
                }
                normal_start = i;
                /* Don't increment i, reprocess this char */
            }
            break;

        case OSC_STATE_IN_OSC:
            if (c == '\007') {
                /* BEL terminator */
                dispatch_osc(r);
                r->state = OSC_STATE_NORMAL;
                normal_start = i + 1;
                i++;
            } else if (c == '\033') {
                r->state = OSC_STATE_OSC_ESC_END;
                i++;
            } else {
                osc_buf_append(r, c);
                i++;
            }
            break;

        case OSC_STATE_OSC_ESC_END:
            if (c == '\\') {
                /* ST terminator */
                dispatch_osc(r);
                r->state = OSC_STATE_NORMAL;
                normal_start = i + 1;
                i++;
            } else if (c == '\033') {
                /* Another ESC, first was data */
                osc_buf_append(r, '\033');
                i++;
            } else {
                /* ESC was data */
                osc_buf_append(r, '\033');
                osc_buf_append(r, c);
                r->state = OSC_STATE_IN_OSC;
                i++;
            }
            break;
        }
    }

    /* Flush remaining normal data */
    if (r->state == OSC_STATE_NORMAL && i > normal_start &&
        r->default_sink && r->default_sink->ops && r->default_sink->ops->write) {
        r->default_sink->ops->write(r->default_sink,
            data + normal_start, i - normal_start);
    }
}

struct yetty_term_pty_reader_result yetty_term_pty_reader_create(
    struct yetty_platform_pty *pty)
{
    struct yetty_term_pty_reader *r;

    if (!pty)
        return YETTY_ERR(yetty_term_pty_reader, "null pty");

    r = calloc(1, sizeof(struct yetty_term_pty_reader));
    if (!r)
        return YETTY_ERR(yetty_term_pty_reader, "alloc failed");

    r->pty = pty;
    r->state = OSC_STATE_NORMAL;

    return YETTY_OK(yetty_term_pty_reader, r);
}

void yetty_term_pty_reader_destroy(struct yetty_term_pty_reader *reader)
{
    if (!reader)
        return;
    free(reader->osc_buf);
    free(reader);
}

void yetty_term_pty_reader_register_default_sink(
    struct yetty_term_pty_reader *reader,
    struct yetty_term_terminal_layer *layer)
{
    if (reader)
        reader->default_sink = layer;
}

void yetty_term_pty_reader_register_osc_sink(
    struct yetty_term_pty_reader *reader,
    int vendor_id,
    struct yetty_term_terminal_layer *layer)
{
    if (!reader || reader->osc_sink_count >= MAX_OSC_SINKS)
        return;

    reader->osc_sinks[reader->osc_sink_count].vendor_id = vendor_id;
    reader->osc_sinks[reader->osc_sink_count].layer = layer;
    reader->osc_sink_count++;
}

int yetty_term_pty_reader_read(struct yetty_term_pty_reader *reader)
{
    char buf[PTY_READ_BUF_SIZE];
    size_t total = 0;

    if (!reader || !reader->pty || !reader->pty->ops || !reader->pty->ops->read)
        return -1;

    struct yetty_core_size_result res;
    while ((res = reader->pty->ops->read(reader->pty, buf, sizeof(buf))),
           YETTY_IS_OK(res) && res.value > 0) {
        process_data(reader, buf, res.value);
        total += res.value;

        /* Keep reading if in OSC (can be huge) */
        if (reader->state != OSC_STATE_NORMAL)
            continue;

        /* Yield after max chunk */
        if (total >= PTY_READ_MAX_CHUNK)
            break;
    }

    return (int)total;
}
