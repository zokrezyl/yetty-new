#ifndef YDEBUG_H
#define YDEBUG_H

#include <stdio.h>
#include <stdlib.h>

static inline int ydebug_enabled(void) {
    static int checked = 0;
    static int enabled = 0;
    if (!checked) {
        enabled = getenv("YDEBUG") != NULL;
        checked = 1;
    }
    return enabled;
}

#define ydebug(fmt, ...) do { \
    if (ydebug_enabled()) \
        fprintf(stderr, "[YDEBUG] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#endif /* YDEBUG_H */
