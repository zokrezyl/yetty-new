/*
 * win32-compat.h — minimal Win32 shims for tinyemu.
 *
 * Included via -include on Windows builds (see tinyemu.cmake). Provides:
 *   - pthread_mutex_t / init / lock / unlock / destroy via CRITICAL_SECTION
 *   - gettimeofday() and struct timeval via GetSystemTimeAsFileTime
 *   - usleep()    via Sleep()
 *   - sleep()     via Sleep()
 *   - typedefs that fill in for missing POSIX headers (sys/time.h, unistd.h)
 *
 * This is the smallest set that lets tinyemu's CPU/virtio core compile on
 * Windows. SLIRP networking is *not* covered here — it is disabled on
 * Windows in tinyemu.cmake.
 */

#ifndef TINYEMU_WIN32_COMPAT_H
#define TINYEMU_WIN32_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* winsock2.h must come before windows.h, and we include it for fd_set
 * (used by virtio.h's EthernetDevice select_fill/select_poll signatures). */
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc/free — must be in scope before our shims use them */

#ifndef _SSIZE_T_DEFINED
typedef long long ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* access() test modes (POSIX). MSVC's _access uses the same numeric values. */
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif
#ifndef access
#define access _access
#endif

/* MAX/MIN come from <sys/param.h> on Linux/BSD; provide them here. */
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* GCC's __builtin_expect → identity. */
#ifndef __builtin_expect
#define __builtin_expect(expr, val) (expr)
#endif

/* GCC's __builtin_clz / __builtin_clzll → MSVC _BitScanReverse intrinsics. */
#include <intrin.h>
static __inline int __builtin_clz(unsigned int x)
{
    unsigned long r;
    return _BitScanReverse(&r, x) ? (31 - (int)r) : 32;
}
static __inline int __builtin_clzll(unsigned long long x)
{
    unsigned long r;
    return _BitScanReverse64(&r, x) ? (63 - (int)r) : 64;
}
static __inline int __builtin_ctz(unsigned int x)
{
    unsigned long r;
    return _BitScanForward(&r, x) ? (int)r : 32;
}
static __inline int __builtin_ctzll(unsigned long long x)
{
    unsigned long r;
    return _BitScanForward64(&r, x) ? (int)r : 64;
}

/* POSIX 64-bit ftell/fseek. */
#ifndef ftello
#define ftello _ftelli64
#endif
#ifndef fseeko
#define fseeko _fseeki64
#endif

/* MSVC has no __attribute__; tinyemu sources sprinkle a handful of
 * (unused), (aligned), (packed). Drop them on MSVC. clang on Windows
 * understands __attribute__ natively, so only blank out for MSVC proper. */
#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(x)
#endif

/* GCC's __atomic_* builtins → C11 stdatomic.h (enabled by /std:clatest +
 * /experimental:c11atomics). typeof is C23 (also /std:clatest). */
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdatomic.h>
#define __ATOMIC_RELAXED memory_order_relaxed
#define __ATOMIC_CONSUME memory_order_consume
#define __ATOMIC_ACQUIRE memory_order_acquire
#define __ATOMIC_RELEASE memory_order_release
#define __ATOMIC_ACQ_REL memory_order_acq_rel
#define __ATOMIC_SEQ_CST memory_order_seq_cst

#define __atomic_load_n(p, m) \
    atomic_load_explicit((_Atomic typeof(*(p)) *)(p), (m))
#define __atomic_store_n(p, v, m) \
    atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_exchange_n(p, v, m) \
    atomic_exchange_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_fetch_add(p, v, m) \
    atomic_fetch_add_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_fetch_sub(p, v, m) \
    atomic_fetch_sub_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_fetch_and(p, v, m) \
    atomic_fetch_and_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_fetch_or(p, v, m) \
    atomic_fetch_or_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_fetch_xor(p, v, m) \
    atomic_fetch_xor_explicit((_Atomic typeof(*(p)) *)(p), (v), (m))
#define __atomic_compare_exchange_n(p, e, d, weak, sm, fm) \
    atomic_compare_exchange_strong_explicit( \
        (_Atomic typeof(*(p)) *)(p), (e), (d), (sm), (fm))
#endif

/* sysconf(_SC_NPROCESSORS_ONLN) → GetSystemInfo. */
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
static __inline long sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return -1;
}
#endif

/* ---- pthread mutex / cond / thread shim --------------------------------*/

/* Use SRWLOCK (not CRITICAL_SECTION) so PTHREAD_MUTEX_INITIALIZER = {0}
 * works for static globals, matching POSIX semantics. SRWLOCK is the
 * Win32 primitive that natively supports zero-initialization. */
typedef SRWLOCK            pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE             pthread_t;
typedef int pthread_mutexattr_t;   /* unused, signature-only */
typedef int pthread_condattr_t;    /* unused */
typedef int pthread_attr_t;        /* unused */

#define PTHREAD_MUTEX_INITIALIZER {0}

static __inline int pthread_mutex_init(pthread_mutex_t *m,
                                       const pthread_mutexattr_t *attr)
{
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}

static __inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    AcquireSRWLockExclusive(m);
    return 0;
}

static __inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    ReleaseSRWLockExclusive(m);
    return 0;
}

static __inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    /* SRWLOCK has no destroy. */
    return 0;
}

static __inline int pthread_cond_init(pthread_cond_t *c,
                                      const pthread_condattr_t *attr)
{
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}

static __inline int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    /* No destroy API for CONDITION_VARIABLE on Win32. */
    return 0;
}

static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    /* SRWLOCK + CONDITION_VARIABLE → SleepConditionVariableSRW. */
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}

static __inline int pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable(c);
    return 0;
}

static __inline int pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable(c);
    return 0;
}

/* pthread_create thunk: POSIX expects void *(*)(void *); Win32 wants
 * DWORD WINAPI(*)(LPVOID). We malloc a small shim that adapts. */
typedef struct {
    void *(*fn)(void *);
    void  *arg;
} _yetty_pthread_thunk;

static DWORD WINAPI _yetty_pthread_run(LPVOID p)
{
    _yetty_pthread_thunk *t = (_yetty_pthread_thunk *)p;
    void *(*fn)(void *) = t->fn;
    void  *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}

static __inline int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                                   void *(*fn)(void *), void *arg)
{
    (void)attr;
    _yetty_pthread_thunk *t = (_yetty_pthread_thunk *)malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn; t->arg = arg;
    HANDLE h = CreateThread(NULL, 0, _yetty_pthread_run, t, 0, NULL);
    if (!h) { free(t); return -1; }
    *th = h;
    return 0;
}

static __inline int pthread_join(pthread_t th, void **retval)
{
    if (retval) *retval = NULL;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    return 0;
}

/* ---- gettimeofday / struct timeval --------------------------------------*/

/* struct timeval is already provided by winsock2.h. */

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* localtime_r(time_t*, tm*) → MSVC's localtime_s(tm*, time_t*). */
#include <time.h>
static __inline struct tm *localtime_r_win32(const time_t *t, struct tm *out)
{
    if (localtime_s(out, t) != 0) return NULL;
    return out;
}
#define localtime_r(t, out) localtime_r_win32((t), (out))

/* POSIX struct tm has tm_gmtoff (seconds east of UTC); MSVC's doesn't.
 * Map it onto tm_isdst so machine.c compiles; the resulting RTC offset
 * default ends up as 0 (UTC), which is fine for first boot. */
#define tm_gmtoff tm_isdst

static __inline int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    /* 100-ns intervals since 1601-01-01 to seconds/microseconds since
     * the UNIX epoch (1970-01-01). */
    static const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    uint64_t now_us = (u.QuadPart - EPOCH_DIFF_100NS) / 10ULL;
    tv->tv_sec  = (long)(now_us / 1000000ULL);
    tv->tv_usec = (long)(now_us % 1000000ULL);
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

/* ---- clock_gettime ------------------------------------------------------*/

/* struct timespec is provided by MSVC's <time.h> (C11+). Don't redefine. */

#ifndef CLOCK_MONOTONIC
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;
#endif

static __inline int clock_gettime(clockid_t clk, struct timespec *ts)
{
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec  = (long)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
        return 0;
    }
    /* CLOCK_REALTIME */
    static const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    uint64_t now_100ns = u.QuadPart - EPOCH_DIFF_100NS;
    ts->tv_sec  = (long)(now_100ns / 10000000ULL);
    ts->tv_nsec = (long)((now_100ns % 10000000ULL) * 100ULL);
    return 0;
}

/* ---- sleep / usleep -----------------------------------------------------*/

static __inline unsigned int sleep(unsigned int sec)
{
    Sleep(sec * 1000U);
    return 0;
}

static __inline int usleep(uint32_t us)
{
    /* Sleep granularity is 1ms; round up. */
    Sleep((us + 999U) / 1000U);
    return 0;
}

/* ---- POSIX file IO aliases (read/write/close/lseek) ---------------------*/

#define read   _read
#define write  _write
#define close  _close
#define lseek  _lseeki64
#define open   _open
#define dup    _dup
#define dup2   _dup2

/* MSVC has _fileno; some POSIX code expects fileno(). */
#ifndef fileno
#define fileno _fileno
#endif

/* POSIX pipe(int fds[2]) → Win32 _pipe with default 64KB buffer + binary mode. */
#include <fcntl.h>
static __inline int _yetty_pipe2(int fds[2])
{
    return _pipe(fds, 65536, _O_BINARY);
}
#define pipe(fds) _yetty_pipe2(fds)

/* fcntl(F_SETFL, O_NONBLOCK) is a no-op for Win32 _pipe handles — they're
 * blocking. Code that depends on non-blocking semantics needs platform
 * branches. Stub returns success. */
#ifndef F_SETFL
#define F_SETFL    4
#define F_GETFL    3
#define O_NONBLOCK 04000
#endif
static __inline int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }

/* mkdir(path, mode) → _mkdir(path); Win32 ignores mode bits. */
static __inline int _yetty_mkdir2(const char *p, int mode) { (void)mode; return _mkdir(p); }
#define mkdir(p, m) _yetty_mkdir2((p), (m))

#endif /* _WIN32 */

#endif /* TINYEMU_WIN32_COMPAT_H */
