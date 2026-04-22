/*
 * SMP support for TinyEMU
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "cutils.h"
#include "smp.h"
#include "ydebug.h"

#define INVALID_RESERVATION ((uint64_t)-1)

int smp_get_host_cpu_count(void)
{
#if defined(__APPLE__)
    /* macOS and iOS */
    int count;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.ncpu", &count, &size, NULL, 0) == 0 && count > 0)
        return count;
    return 1;
#else
    /* Linux and other POSIX - use sysconf from unistd.h */
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0)
        return (int)count;
    return 1;
#endif
}

/*
 * Reservation System
 */

void smp_reservation_init(SMPReservationSet *rs, int num_cpus)
{
    rs->num_cpus = num_cpus;
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        atomic_store(&rs->addr[i], INVALID_RESERVATION);
    }
}

void smp_reservation_set(SMPReservationSet *rs, int cpu_id, uint64_t addr)
{
    if (cpu_id >= 0 && cpu_id < rs->num_cpus) {
        atomic_store(&rs->addr[cpu_id], addr);
    }
}

int smp_reservation_check(SMPReservationSet *rs, int cpu_id, uint64_t addr)
{
    if (cpu_id >= 0 && cpu_id < rs->num_cpus) {
        return atomic_load(&rs->addr[cpu_id]) == addr;
    }
    return 0;
}

int smp_reservation_check_and_clear(SMPReservationSet *rs, int cpu_id, uint64_t addr)
{
    if (cpu_id >= 0 && cpu_id < rs->num_cpus) {
        uint64_t expected = addr;
        /* Atomically: if reservation == addr, set to INVALID and return 1
         * This ensures only ONE CPU can successfully claim a reservation */
        return atomic_compare_exchange_strong(&rs->addr[cpu_id], &expected,
                                              INVALID_RESERVATION);
    }
    return 0;
}

void smp_reservation_clear(SMPReservationSet *rs, int cpu_id)
{
    if (cpu_id >= 0 && cpu_id < rs->num_cpus) {
        atomic_store(&rs->addr[cpu_id], INVALID_RESERVATION);
    }
}

void smp_reservation_invalidate(SMPReservationSet *rs, uint64_t addr)
{
    /* Clear all reservations for this address */
    for (int i = 0; i < rs->num_cpus; i++) {
        uint64_t expected = addr;
        atomic_compare_exchange_strong(&rs->addr[i], &expected, INVALID_RESERVATION);
    }
}

/*
 * Atomic Operations - 32-bit
 */

uint32_t smp_atomic_swap32(uint32_t *ptr, uint32_t val)
{
    return __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST);
}

uint32_t smp_atomic_add32(uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST);
}

uint32_t smp_atomic_xor32(uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_xor(ptr, val, __ATOMIC_SEQ_CST);
}

uint32_t smp_atomic_and32(uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_and(ptr, val, __ATOMIC_SEQ_CST);
}

uint32_t smp_atomic_or32(uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_or(ptr, val, __ATOMIC_SEQ_CST);
}

int32_t smp_atomic_min32(int32_t *ptr, int32_t val)
{
    int32_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val < old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

int32_t smp_atomic_max32(int32_t *ptr, int32_t val)
{
    int32_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val > old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

uint32_t smp_atomic_minu32(uint32_t *ptr, uint32_t val)
{
    uint32_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val < old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

uint32_t smp_atomic_maxu32(uint32_t *ptr, uint32_t val)
{
    uint32_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val > old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

int smp_atomic_cmpxchg32(uint32_t *ptr, uint32_t expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/*
 * Atomic Operations - 64-bit
 */

uint64_t smp_atomic_swap64(uint64_t *ptr, uint64_t val)
{
    return __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST);
}

uint64_t smp_atomic_add64(uint64_t *ptr, uint64_t val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST);
}

uint64_t smp_atomic_xor64(uint64_t *ptr, uint64_t val)
{
    return __atomic_fetch_xor(ptr, val, __ATOMIC_SEQ_CST);
}

uint64_t smp_atomic_and64(uint64_t *ptr, uint64_t val)
{
    return __atomic_fetch_and(ptr, val, __ATOMIC_SEQ_CST);
}

uint64_t smp_atomic_or64(uint64_t *ptr, uint64_t val)
{
    return __atomic_fetch_or(ptr, val, __ATOMIC_SEQ_CST);
}

int64_t smp_atomic_min64(int64_t *ptr, int64_t val)
{
    int64_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val < old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

int64_t smp_atomic_max64(int64_t *ptr, int64_t val)
{
    int64_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val > old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

uint64_t smp_atomic_minu64(uint64_t *ptr, uint64_t val)
{
    uint64_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val < old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

uint64_t smp_atomic_maxu64(uint64_t *ptr, uint64_t val)
{
    uint64_t old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
    while (val > old) {
        if (__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    return old;
}

int smp_atomic_cmpxchg64(uint64_t *ptr, uint64_t expected, uint64_t desired)
{
    return __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/*
 * Atomic Operations - 128-bit (lock-based fallback)
 */

#if defined(__SIZEOF_INT128__)

typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;

static pthread_mutex_t smp_atomic128_lock = PTHREAD_MUTEX_INITIALIZER;

uint128_t smp_atomic_swap128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    *ptr = val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

uint128_t smp_atomic_add128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    *ptr = old + val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

uint128_t smp_atomic_xor128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    *ptr = old ^ val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

uint128_t smp_atomic_and128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    *ptr = old & val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

uint128_t smp_atomic_or128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    *ptr = old | val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

int128_t smp_atomic_min128(int128_t *ptr, int128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    int128_t old = *ptr;
    if (val < old)
        *ptr = val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

int128_t smp_atomic_max128(int128_t *ptr, int128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    int128_t old = *ptr;
    if (val > old)
        *ptr = val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

uint128_t smp_atomic_minu128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    if (val < old)
        *ptr = val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

uint128_t smp_atomic_maxu128(uint128_t *ptr, uint128_t val)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    uint128_t old = *ptr;
    if (val > old)
        *ptr = val;
    pthread_mutex_unlock(&smp_atomic128_lock);
    return old;
}

int smp_atomic_cmpxchg128(uint128_t *ptr, uint128_t expected, uint128_t desired)
{
    pthread_mutex_lock(&smp_atomic128_lock);
    int success = 0;
    if (*ptr == expected) {
        *ptr = desired;
        success = 1;
    }
    pthread_mutex_unlock(&smp_atomic128_lock);
    return success;
}

#endif /* __SIZEOF_INT128__ */

/*
 * CPU Thread Management
 */

/* Forward declaration - implemented in riscv_machine.c */
extern void riscv_cpu_thread_func(SMPCPUThread *t);

static void *cpu_thread_entry(void *arg)
{
    SMPCPUThread *t = arg;
    /* Memory barrier to ensure we see all initialization from main thread */
    atomic_thread_fence(memory_order_seq_cst);
    ydebug("cpu_thread_entry: CPU %d starting", t->cpu_id);
    riscv_cpu_thread_func(t);
    ydebug("cpu_thread_entry: CPU %d exiting", t->cpu_id);
    return NULL;
}

SMPState *smp_init(int num_cpus)
{
    if (num_cpus > SMP_MAX_CPUS)
        num_cpus = SMP_MAX_CPUS;
    if (num_cpus < 1)
        num_cpus = 1;

    SMPState *smp = mallocz(sizeof(SMPState));
    smp->num_cpus = num_cpus;
    smp_reservation_init(&smp->reservations, num_cpus);
    pthread_mutex_init(&smp->device_lock, NULL);
    atomic_store(&smp->amo_lock, 0);
    atomic_store(&smp->tlb_flush_gen, 0);

    for (int i = 0; i < num_cpus; i++) {
        smp->threads[i].cpu_id = i;
        atomic_store(&smp->threads[i].running, 0);
        pthread_mutex_init(&smp->threads[i].wakeup_mutex, NULL);
        pthread_cond_init(&smp->threads[i].wakeup_cond, NULL);
        smp->threads[i].wakeup_pending = 0;
    }

    return smp;
}

void smp_start_cpus(SMPState *smp, struct VirtMachine *vm)
{
    ydebug("smp_start_cpus: starting %d CPUs", smp->num_cpus);
    for (int i = 0; i < smp->num_cpus; i++) {
        smp->threads[i].vm = vm;
        atomic_store(&smp->threads[i].running, 1);
        pthread_create(&smp->threads[i].thread, NULL, cpu_thread_entry, &smp->threads[i]);
        ydebug("smp_start_cpus: created thread for CPU %d", i);
    }
}

void smp_stop_cpus(SMPState *smp)
{
    /* First mark all as not running */
    for (int i = 0; i < smp->num_cpus; i++) {
        atomic_store(&smp->threads[i].running, 0);
    }
    /* Wake up any that might be blocked on condvar */
    for (int i = 0; i < smp->num_cpus; i++) {
        pthread_mutex_lock(&smp->threads[i].wakeup_mutex);
        pthread_cond_signal(&smp->threads[i].wakeup_cond);
        pthread_mutex_unlock(&smp->threads[i].wakeup_mutex);
    }
    /* Now join */
    for (int i = 0; i < smp->num_cpus; i++) {
        pthread_join(smp->threads[i].thread, NULL);
    }
}

void smp_free(SMPState *smp)
{
    if (smp) {
        for (int i = 0; i < smp->num_cpus; i++) {
            pthread_mutex_destroy(&smp->threads[i].wakeup_mutex);
            pthread_cond_destroy(&smp->threads[i].wakeup_cond);
        }
        pthread_mutex_destroy(&smp->device_lock);
        free(smp);
    }
}

void smp_wakeup_cpu(SMPState *smp, int cpu_id)
{
    if (smp && cpu_id >= 0 && cpu_id < smp->num_cpus) {
        ydebug("SYNC: SIGNAL cpu%d", cpu_id);
        atomic_thread_fence(memory_order_seq_cst);
        pthread_mutex_lock(&smp->threads[cpu_id].wakeup_mutex);
        smp->threads[cpu_id].wakeup_pending = 1;
        pthread_cond_signal(&smp->threads[cpu_id].wakeup_cond);
        pthread_mutex_unlock(&smp->threads[cpu_id].wakeup_mutex);
    }
}
