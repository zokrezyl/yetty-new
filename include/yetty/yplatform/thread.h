/*
 * yplatform/thread.h - Cross-platform threading abstraction
 */

#ifndef YETTY_YPLATFORM_THREAD_H
#define YETTY_YPLATFORM_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque thread handle */
typedef struct ythread ythread_t;

/* Thread function signature: returns 0 on success */
typedef int (*ythread_func_t)(void *arg);

/* Thread */
ythread_t *ythread_create(ythread_func_t func, void *arg);
int ythread_join(ythread_t *thread);

/* Opaque mutex handle */
typedef struct ymutex ymutex_t;

/* Mutex */
ymutex_t *ymutex_create(void);
void ymutex_destroy(ymutex_t *m);
void ymutex_lock(ymutex_t *m);
void ymutex_unlock(ymutex_t *m);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YPLATFORM_THREAD_H */
