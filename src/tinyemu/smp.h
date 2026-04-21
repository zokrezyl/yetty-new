/*
 * SMP support for TinyEMU
 *
 * Provides multi-core support with proper atomic operations
 * and LR/SC reservation handling across cores.
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include "cutils.h"

/* Maximum number of CPUs */
#define SMP_MAX_CPUS 32

/* Get number of host CPUs */
int smp_get_host_cpu_count(void);

/*
 * LR/SC Reservation System
 *
 * Each CPU can have one active reservation. When any CPU writes
 * to a reserved address, all reservations for that address are invalidated.
 */

typedef struct {
    _Atomic uint64_t addr[SMP_MAX_CPUS];  /* Reserved address per CPU, -1 = invalid */
    int num_cpus;
} SMPReservationSet;

void smp_reservation_init(SMPReservationSet *rs, int num_cpus);
void smp_reservation_set(SMPReservationSet *rs, int cpu_id, uint64_t addr);
int smp_reservation_check(SMPReservationSet *rs, int cpu_id, uint64_t addr);
int smp_reservation_check_and_clear(SMPReservationSet *rs, int cpu_id, uint64_t addr);
void smp_reservation_clear(SMPReservationSet *rs, int cpu_id);
void smp_reservation_invalidate(SMPReservationSet *rs, uint64_t addr);

/*
 * Atomic Memory Operations
 *
 * These wrap the host's atomic operations for RISC-V AMO instructions.
 */

uint32_t smp_atomic_swap32(uint32_t *ptr, uint32_t val);
uint32_t smp_atomic_add32(uint32_t *ptr, uint32_t val);
uint32_t smp_atomic_xor32(uint32_t *ptr, uint32_t val);
uint32_t smp_atomic_and32(uint32_t *ptr, uint32_t val);
uint32_t smp_atomic_or32(uint32_t *ptr, uint32_t val);
int32_t smp_atomic_min32(int32_t *ptr, int32_t val);
int32_t smp_atomic_max32(int32_t *ptr, int32_t val);
uint32_t smp_atomic_minu32(uint32_t *ptr, uint32_t val);
uint32_t smp_atomic_maxu32(uint32_t *ptr, uint32_t val);
/* CAS for SC instruction: returns 1 if successful, 0 if failed */
int smp_atomic_cmpxchg32(uint32_t *ptr, uint32_t expected, uint32_t desired);

uint64_t smp_atomic_swap64(uint64_t *ptr, uint64_t val);
uint64_t smp_atomic_add64(uint64_t *ptr, uint64_t val);
uint64_t smp_atomic_xor64(uint64_t *ptr, uint64_t val);
uint64_t smp_atomic_and64(uint64_t *ptr, uint64_t val);
uint64_t smp_atomic_or64(uint64_t *ptr, uint64_t val);
int64_t smp_atomic_min64(int64_t *ptr, int64_t val);
int64_t smp_atomic_max64(int64_t *ptr, int64_t val);
uint64_t smp_atomic_minu64(uint64_t *ptr, uint64_t val);
uint64_t smp_atomic_maxu64(uint64_t *ptr, uint64_t val);
int smp_atomic_cmpxchg64(uint64_t *ptr, uint64_t expected, uint64_t desired);

/* 128-bit operations (lock-based fallback since native 128-bit atomics are rare) */
#if defined(__SIZEOF_INT128__)
typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;
uint128_t smp_atomic_swap128(uint128_t *ptr, uint128_t val);
uint128_t smp_atomic_add128(uint128_t *ptr, uint128_t val);
uint128_t smp_atomic_xor128(uint128_t *ptr, uint128_t val);
uint128_t smp_atomic_and128(uint128_t *ptr, uint128_t val);
uint128_t smp_atomic_or128(uint128_t *ptr, uint128_t val);
int128_t smp_atomic_min128(int128_t *ptr, int128_t val);
int128_t smp_atomic_max128(int128_t *ptr, int128_t val);
uint128_t smp_atomic_minu128(uint128_t *ptr, uint128_t val);
uint128_t smp_atomic_maxu128(uint128_t *ptr, uint128_t val);
int smp_atomic_cmpxchg128(uint128_t *ptr, uint128_t expected, uint128_t desired);
#endif

/*
 * CPU Thread Management
 */

struct RISCVCPUState;
struct VirtMachine;

typedef struct {
    pthread_t thread;
    struct RISCVCPUState *cpu;
    struct VirtMachine *vm;
    int cpu_id;
    _Atomic int running;
    /* Condition variable for idle waiting (WFI) */
    pthread_mutex_t wakeup_mutex;
    pthread_cond_t wakeup_cond;
    int wakeup_pending;  /* Set inside mutex to avoid lost wakeups */
} SMPCPUThread;

typedef struct SMPState {
    SMPCPUThread threads[SMP_MAX_CPUS];
    int num_cpus;
    SMPReservationSet reservations;
    pthread_mutex_t device_lock;  /* Lock for device access */
    _Atomic int amo_lock;         /* Spinlock for non-atomic AMO fallback */
    _Atomic uint64_t tlb_flush_gen; /* Global TLB generation for SMP shootdown */
} SMPState;

/* Spinlock for AMO operations that can't use native atomics */
static inline void smp_amo_lock(SMPState *smp) {
    int expected = 0;
    while (!atomic_compare_exchange_weak(&smp->amo_lock, &expected, 1)) {
        expected = 0;
    }
}

static inline void smp_amo_unlock(SMPState *smp) {
    atomic_store(&smp->amo_lock, 0);
}

SMPState *smp_init(int num_cpus);
void smp_start_cpus(SMPState *smp, struct VirtMachine *vm);
void smp_stop_cpus(SMPState *smp);
void smp_free(SMPState *smp);

/* Wake up a CPU that is waiting on WFI */
void smp_wakeup_cpu(SMPState *smp, int cpu_id);

/* Called by memory write to invalidate reservations */
static inline void smp_on_store(SMPState *smp, uint64_t addr)
{
    if (smp) {
        smp_reservation_invalidate(&smp->reservations, addr);
    }
}

#endif /* SMP_H */
