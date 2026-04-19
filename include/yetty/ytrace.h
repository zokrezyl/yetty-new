#ifndef YTRACE_C_H
#define YTRACE_C_H

/*
 * ytrace-c: C implementation of switchable trace points
 *
 * Each trace point has a static bool that can be toggled at runtime.
 * When disabled, only a single if-check is executed (minimal overhead).
 *
 * Usage:
 *   ytrace("processing item %d", item_id);
 *   ydebug("buffer size: %zu", size);
 *   yinfo("connection established");
 *   ywarn("timeout exceeded: %dms", timeout);
 *   yerror("failed to open file: %s", path);
 *
 * Control:
 *   ytrace_set_all_enabled(true);           // enable all trace points
 *   ytrace_set_level_enabled("trace", false); // disable by level
 *   ytrace_set_file_enabled("foo.c", true);   // enable by file
 *
 * Environment:
 *   YTRACE_DEFAULT_ON=yes  - enable all trace points by default
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time master switch */
#ifndef YTRACE_C_ENABLED
#define YTRACE_C_ENABLED 1
#endif

/* Per-level compile-time switches (set to 0 to completely remove) */
#ifndef YTRACE_C_ENABLE_TRACE
#define YTRACE_C_ENABLE_TRACE 1
#endif

#ifndef YTRACE_C_ENABLE_DEBUG
#define YTRACE_C_ENABLE_DEBUG 1
#endif

#ifndef YTRACE_C_ENABLE_INFO
#define YTRACE_C_ENABLE_INFO 1
#endif

#ifndef YTRACE_C_ENABLE_WARN
#define YTRACE_C_ENABLE_WARN 1
#endif

#ifndef YTRACE_C_ENABLE_ERROR
#define YTRACE_C_ENABLE_ERROR 1
#endif

#if YTRACE_C_ENABLED

/* Maximum number of trace points (increase if needed) */
#ifndef YTRACE_C_MAX_POINTS
#define YTRACE_C_MAX_POINTS 4096
#endif

/* Trace point information */
typedef struct {
    bool* enabled;
    const char* file;
    int line;
    const char* function;
    const char* level;
    const char* message;
} ytrace_point_t;

/* Initialize the trace system (call once at startup, optional - auto-inits on first use) */
void ytrace_init(void);

/* Shutdown and cleanup */
void ytrace_shutdown(void);

/* Register a trace point - returns initial enabled state */
bool ytrace_register(bool* enabled, const char* file, int line,
                     const char* func, const char* level, const char* message);

/* Output a trace message (called when trace point is enabled) */
// TODO: WE DO NOT WANT TO SEE ANY PLATFORM SPECIFIC CODE! ADD THEN WITH IFDEF!
void ytrace_output(const char* level, const char* file, int line,
                   const char* func, const char* fmt, ...)
#ifndef _MSC_VER
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/* Control functions */
void ytrace_set_all_enabled(bool enabled);
void ytrace_set_level_enabled(const char* level, bool enabled);
void ytrace_set_file_enabled(const char* file, bool enabled);
void ytrace_set_function_enabled(const char* function, bool enabled);

/* Query functions */
size_t ytrace_get_point_count(void);
const ytrace_point_t* ytrace_get_points(void);

/* List all trace points to stderr */
void ytrace_list(void);

/*
 * Trace macros
 *
 * Each macro creates a function-local static bool that:
 * 1. Is initialized by calling ytrace_register() on first execution
 * 2. Can be toggled later via the control functions
 * 3. Guards the actual trace output with a simple if-check
 */

#if YTRACE_C_ENABLE_TRACE
#define ytrace(fmt, ...) \
    do { \
        static bool _ytrace_enabled_ = false; \
        static bool _ytrace_registered_ = false; \
        if (!_ytrace_registered_) { \
            _ytrace_enabled_ = ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "trace", fmt); \
            _ytrace_registered_ = true; \
        } \
        if (_ytrace_enabled_) { \
            ytrace_output("trace", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define ytrace(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_DEBUG
#define ydebug(fmt, ...) \
    do { \
        static bool _ytrace_enabled_ = false; \
        static bool _ytrace_registered_ = false; \
        if (!_ytrace_registered_) { \
            _ytrace_enabled_ = ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "debug", fmt); \
            _ytrace_registered_ = true; \
        } \
        if (_ytrace_enabled_) { \
            ytrace_output("debug", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define ydebug(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_INFO
#define yinfo(fmt, ...) \
    do { \
        static bool _ytrace_enabled_ = false; \
        static bool _ytrace_registered_ = false; \
        if (!_ytrace_registered_) { \
            _ytrace_enabled_ = ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "info", fmt); \
            _ytrace_registered_ = true; \
        } \
        if (_ytrace_enabled_) { \
            ytrace_output("info", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define yinfo(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_WARN
#define ywarn(fmt, ...) \
    do { \
        static bool _ytrace_enabled_ = false; \
        static bool _ytrace_registered_ = false; \
        if (!_ytrace_registered_) { \
            _ytrace_enabled_ = ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "warn", fmt); \
            _ytrace_registered_ = true; \
        } \
        if (_ytrace_enabled_) { \
            ytrace_output("warn", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define ywarn(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_ERROR
#define yerror(fmt, ...) \
    do { \
        static bool _ytrace_enabled_ = false; \
        static bool _ytrace_registered_ = false; \
        if (!_ytrace_registered_) { \
            _ytrace_enabled_ = ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "error", fmt); \
            _ytrace_registered_ = true; \
        } \
        if (_ytrace_enabled_) { \
            ytrace_output("error", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define yerror(fmt, ...) ((void)0)
#endif

/* Generic log macro with explicit level */
#define ylog(lvl, fmt, ...) \
    do { \
        static bool _ytrace_enabled_ = false; \
        static bool _ytrace_registered_ = false; \
        if (!_ytrace_registered_) { \
            _ytrace_enabled_ = ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, lvl, fmt); \
            _ytrace_registered_ = true; \
        } \
        if (_ytrace_enabled_) { \
            ytrace_output(lvl, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#else /* !YTRACE_C_ENABLED */

/* All macros become no-ops when ytrace-c is disabled */
#define ytrace(fmt, ...) ((void)0)
#define ydebug(fmt, ...) ((void)0)
#define yinfo(fmt, ...)  ((void)0)
#define ywarn(fmt, ...)  ((void)0)
#define yerror(fmt, ...) ((void)0)
#define ylog(lvl, fmt, ...) ((void)0)

static inline void ytrace_init(void) {}
static inline void ytrace_shutdown(void) {}
static inline void ytrace_set_all_enabled(bool enabled) { (void)enabled; }
static inline void ytrace_set_level_enabled(const char* level, bool enabled) { (void)level; (void)enabled; }
static inline void ytrace_set_file_enabled(const char* file, bool enabled) { (void)file; (void)enabled; }
static inline void ytrace_set_function_enabled(const char* function, bool enabled) { (void)function; (void)enabled; }
static inline size_t ytrace_get_point_count(void) { return 0; }
static inline const ytrace_point_t* ytrace_get_points(void) { return NULL; }
static inline void ytrace_list(void) {}

#endif /* YTRACE_C_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* YTRACE_C_H */
