#ifndef YETTY_CORE_RESULT_H
#define YETTY_CORE_RESULT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error info */
struct yetty_core_error {
    const char *msg;
};

/* Generate type-specific result struct: struct <name>_result */
#define YETTY_RESULT_DECLARE(name, value_type) \
    struct name##_result { \
        int ok; \
        union { \
            value_type value; \
            struct yetty_core_error error; \
        }; \
    }

/* Common result types in core namespace */
YETTY_RESULT_DECLARE(yetty_core_void, int);
YETTY_RESULT_DECLARE(yetty_core_int, int);
YETTY_RESULT_DECLARE(yetty_core_size, size_t);

/* Create success result (void) */
#define YETTY_OK_VOID() \
    ((struct yetty_core_void_result){.ok = 1, .value = 0})

/* Create success result with value */
#define YETTY_OK(name, val) \
    ((struct name##_result){.ok = 1, .value = (val)})

/* Create error result */
#define YETTY_ERR(name, err_msg) \
    ((struct name##_result){ \
        .ok = 0, \
        .error = {.msg = (err_msg)}})

/* Check result */
#define YETTY_IS_OK(res) ((res).ok)
#define YETTY_IS_ERR(res) (!(res).ok)

#ifdef __cplusplus
}
#endif

#endif /* YETTY_CORE_RESULT_H */
