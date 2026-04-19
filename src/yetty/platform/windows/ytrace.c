/*
 * ytrace-c: C implementation of switchable trace points (Windows)
 */

#include <yetty/ytrace.h>

#if YTRACE_C_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <io.h>

/* Global state */
static ytrace_point_t g_points[YTRACE_C_MAX_POINTS];
static size_t g_point_count = 0;
static CRITICAL_SECTION g_mutex;
static bool g_initialized = false;
static bool g_default_enabled = false;
static volatile LONG g_mutex_initialized = 0;

/* ANSI color codes */
#define ANSI_RESET   "\033[0m"
#define ANSI_GRAY    "\033[90m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define ANSI_BOLD    "\033[1m"

/* Check if output is a TTY for color support */
static bool g_use_colors = false;

/* Ensure CRITICAL_SECTION is initialized once */
static void ensure_mutex_initialized(void) {
    if (InterlockedCompareExchange(&g_mutex_initialized, 1, 0) == 0) {
        InitializeCriticalSection(&g_mutex);
    }
}

static void check_color_support(void) {
    const char* no_color = getenv("NO_COLOR");

    if (no_color != NULL) {
        g_use_colors = false;
    } else {
        /* Windows 10+ supports ANSI via virtual terminal processing */
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (hErr != INVALID_HANDLE_VALUE && GetConsoleMode(hErr, &mode)) {
            SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            g_use_colors = true;
        }
    }
}

static const char* level_color(const char* level) {
    if (!g_use_colors) return "";

    if (strcmp(level, "trace") == 0) return ANSI_GRAY;
    if (strcmp(level, "debug") == 0) return ANSI_CYAN;
    if (strcmp(level, "info") == 0)  return ANSI_GREEN;
    if (strcmp(level, "warn") == 0)  return ANSI_YELLOW;
    if (strcmp(level, "error") == 0) return ANSI_RED ANSI_BOLD;
    return "";
}

static const char* color_reset(void) {
    return g_use_colors ? ANSI_RESET : "";
}

void ytrace_init(void) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);

    if (g_initialized) {
        LeaveCriticalSection(&g_mutex);
        return;
    }

    /* Check YTRACE_DEFAULT_ON environment variable */
    const char* default_on = getenv("YTRACE_DEFAULT_ON");
    if (default_on != NULL) {
        g_default_enabled = (strcmp(default_on, "yes") == 0 ||
                            strcmp(default_on, "1") == 0 ||
                            strcmp(default_on, "true") == 0);
    }

    check_color_support();
    g_initialized = true;

    LeaveCriticalSection(&g_mutex);
}

void ytrace_shutdown(void) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);
    g_point_count = 0;
    g_initialized = false;
    LeaveCriticalSection(&g_mutex);
}

bool ytrace_register(bool* enabled, const char* file, int line,
                     const char* func, const char* level, const char* message) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);

    /* Auto-initialize on first registration */
    if (!g_initialized) {
        LeaveCriticalSection(&g_mutex);
        ytrace_init();
        EnterCriticalSection(&g_mutex);
    }

    /* Set initial state */
    *enabled = g_default_enabled;

    /* Register if space available */
    if (g_point_count < YTRACE_C_MAX_POINTS) {
        g_points[g_point_count] = (ytrace_point_t){
            .enabled = enabled,
            .file = file,
            .line = line,
            .function = func,
            .level = level,
            .message = message
        };
        g_point_count++;
    } else {
        /* Overflow - print warning once */
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[ytrace-c] WARNING: max trace points (%d) exceeded\n",
                    YTRACE_C_MAX_POINTS);
            warned = true;
        }
    }

    LeaveCriticalSection(&g_mutex);
    return *enabled;
}

void ytrace_output(const char* level, const char* file, int line,
                   const char* func, const char* fmt, ...) {
    char msg_buf[1024];
    char time_buf[32];

    /* Format the user message */
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Get timestamp with milliseconds */
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    /* Extract basename from file path */
    const char* basename = strrchr(file, '/');
    if (!basename) basename = strrchr(file, '\\');
    if (basename) {
        basename++;
    } else {
        basename = file;
    }

    /* Output format: [HH:MM:SS.mmm] [level] file:line (func): message */
    fprintf(stderr, "[%s] %s[%-5s]%s %s:%d (%s): %s\n",
            time_buf,
            level_color(level), level, color_reset(),
            basename, line, func, msg_buf);
}

void ytrace_set_all_enabled(bool enabled) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);
    for (size_t i = 0; i < g_point_count; i++) {
        *g_points[i].enabled = enabled;
    }
    LeaveCriticalSection(&g_mutex);
}

void ytrace_set_level_enabled(const char* level, bool enabled) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);
    for (size_t i = 0; i < g_point_count; i++) {
        if (strcmp(g_points[i].level, level) == 0) {
            *g_points[i].enabled = enabled;
        }
    }
    LeaveCriticalSection(&g_mutex);
}

void ytrace_set_file_enabled(const char* file, bool enabled) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);
    for (size_t i = 0; i < g_point_count; i++) {
        /* Match full path or basename */
        const char* point_basename = strrchr(g_points[i].file, '/');
        if (!point_basename) point_basename = strrchr(g_points[i].file, '\\');
        point_basename = point_basename ? point_basename + 1 : g_points[i].file;

        if (strcmp(g_points[i].file, file) == 0 ||
            strcmp(point_basename, file) == 0) {
            *g_points[i].enabled = enabled;
        }
    }
    LeaveCriticalSection(&g_mutex);
}

void ytrace_set_function_enabled(const char* function, bool enabled) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);
    for (size_t i = 0; i < g_point_count; i++) {
        if (strcmp(g_points[i].function, function) == 0) {
            *g_points[i].enabled = enabled;
        }
    }
    LeaveCriticalSection(&g_mutex);
}

size_t ytrace_get_point_count(void) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);
    size_t count = g_point_count;
    LeaveCriticalSection(&g_mutex);
    return count;
}

const ytrace_point_t* ytrace_get_points(void) {
    return g_points;
}

void ytrace_list(void) {
    ensure_mutex_initialized();
    EnterCriticalSection(&g_mutex);

    fprintf(stderr, "\n[ytrace-c] Registered trace points: %zu\n", g_point_count);
    fprintf(stderr, "%-4s %-7s %-6s %-30s %-20s %s\n",
            "IDX", "ENABLED", "LEVEL", "FILE:LINE", "FUNCTION", "MESSAGE");
    fprintf(stderr, "%-4s %-7s %-6s %-30s %-20s %s\n",
            "---", "-------", "-----", "-----------------------------",
            "-------------------", "-------");

    for (size_t i = 0; i < g_point_count; i++) {
        const ytrace_point_t* p = &g_points[i];

        /* Extract basename */
        const char* basename = strrchr(p->file, '/');
        if (!basename) basename = strrchr(p->file, '\\');
        basename = basename ? basename + 1 : p->file;

        char loc_buf[32];
        snprintf(loc_buf, sizeof(loc_buf), "%s:%d", basename, p->line);

        /* Truncate message for display */
        char msg_buf[32];
        if (p->message && strlen(p->message) > 0) {
            snprintf(msg_buf, sizeof(msg_buf), "%.28s%s",
                     p->message, strlen(p->message) > 28 ? ".." : "");
        } else {
            msg_buf[0] = '\0';
        }

        fprintf(stderr, "%-4zu %-7s %-6s %-30s %-20s %s\n",
                i,
                *p->enabled ? "ON" : "off",
                p->level,
                loc_buf,
                p->function,
                msg_buf);
    }

    fprintf(stderr, "\n");
    LeaveCriticalSection(&g_mutex);
}

#endif /* YTRACE_C_ENABLED */
