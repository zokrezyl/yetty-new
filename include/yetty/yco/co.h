/*
 * yco/co.h - Thin wrappers around libco with ydebug instrumentation.
 *
 * One-to-one with libco's API, just renamed to yetty_yco_* and with a ydebug
 * statement on every entry. Lets us trace coroutine switches end-to-end while
 * the coroutine layer is still being shaken out.
 *
 * Strip / inline this module once we trust the stack-switch path.
 */

#ifndef YETTY_YCO_CO_H
#define YETTY_YCO_CO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque cothread handle (matches libco's cothread_t). */
typedef void *yetty_yco_thread;

/* Currently-running cothread. */
yetty_yco_thread yetty_yco_active(void);

/* Create a new cothread with stack_size bytes; entry takes no args.
 * Returns NULL on failure. */
yetty_yco_thread yetty_yco_create(unsigned int stack_size, void (*entry)(void));

/* Delete a cothread. The cothread must not be currently running. */
void yetty_yco_delete(yetty_yco_thread thread);

/* Switch execution to `thread`. Returns when somebody switches back. */
void yetty_yco_switch(yetty_yco_thread thread);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YCO_CO_H */
