#include "concurrency/rwlock_latch.h"
#include <immintrin.h>
#include <time.h>
#include <errno.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/futex.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <os/lock.h>
#endif

#define RWLOCK_MAX_SPIN_COUNT 100
#define RWLOCK_MAX_YIELD_COUNT 20

static inline void rwlock_cpu_relax(void) {
#if defined(_MSC_VER)
    _mm_pause();
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

#if defined(__linux__)
static int sys_futex(void *addr, int op, int val, const struct timespec *timeout, void *uaddr2, int val3) {
    return (int)syscall(SYS_futex, addr, op, val, timeout, uaddr2, val3);
}

static void rwlock_futex_wait(atomic_uint *state, uint32_t expected) {
    sys_futex((void *)state, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

static void rwlock_futex_wake_one(atomic_uint *state) {
    sys_futex((void *)state, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void rwlock_futex_wake_all(atomic_uint *state) {
    sys_futex((void *)state, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
#endif

void rwlock_init(rwlock_t *latch) {
    if (!latch) return;
    atomic_init(&latch->state, 0);
    atomic_init(&latch->writer_wait_count, 0);
#if defined(__APPLE__)
    latch->os_mutex = OS_UNFAIR_LOCK_INIT;
    latch->os_cv = (condition_variable_t)OS_FAIR_LOCK_INIT; // Placeholder alignment
#endif
}

void rwlock_destroy(rwlock_t *latch) {
    if (!latch) return;
    atomic_store_explicit(&latch->state, 0, memory_order_relaxed);
    atomic_store_explicit(&latch->writer_wait_count, 0, memory_order_relaxed);
}

bool rwlock_try_rdlock(rwlock_t *latch) {
    if (!latch) return false;
    uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
    
    if (current & RWLOCK_WRITER_MASK) {
        return false;
    }
    
    if (atomic_load_explicit(&latch->writer_wait_count, memory_order_relaxed) > 0) {
        return false;
    }
    
    uint32_t next = current + RWLOCK_READER_INCR;
    if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                memory_order_acquire,
                                                memory_order_relaxed)) {
        return true;
    }
    return false;
}

void rwlock_rdlock(rwlock_t *latch) {
    if (!latch) return;
    
    uint32_t spins = 0;
    uint32_t yields = 0;
    
    while (true) {
        uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
        
        if (!(current & RWLOCK_WRITER_MASK) && 
            atomic_load_explicit(&latch->writer_wait_count, memory_order_relaxed) == 0) {
            uint32_t next = current + RWLOCK_READER_INCR;
            if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                        memory_order_acquire,
                                                        memory_order_relaxed)) {
                return;
            }
            continue;
        }
        
        if (spins < RWLOCK_MAX_SPIN_COUNT) {
            rwlock_cpu_relax();
            spins++;
        } else if (yields < RWLOCK_MAX_YIELD_COUNT) {
#if defined(__linux__) || defined(__APPLE__)
            sched_yield();
#else
            rwlock_cpu_relax();
#endif
            yields++;
        } else {
#if defined(__linux__)
            uint32_t expected = atomic_load_explicit(&latch->state, memory_order_relaxed);
            if (expected & RWLOCK_WRITER_MASK) {
                rwlock_futex_wait(&latch->state, expected);
            }
#elif defined(__APPLE__)
            // Fallback to brief sleep on macOS if contention is extreme
            struct timespec ts = {0, 100000}; // 0.1ms
            nanosleep(&ts, NULL);
#else
            rwlock_cpu_relax();
#endif
            spins = 0;
            yields = 0;
        }
    }
}

bool rwlock_try_wrlock(rwlock_t *latch) {
    if (!latch) return false;
    uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
    
    if (current != 0) {
        return false;
    }
    
    uint32_t next = RWLOCK_WRITER_MASK;
    if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                memory_order_acquire,
                                                memory_order_relaxed)) {
        return true;
    }
    return false;
}

void rwlock_wrlock(rwlock_t *latch) {
    if (!latch) return;
    
    atomic_fetch_add_explicit(&latch->writer_wait_count, 1, memory_order_acquire);
    
    uint32_t spins = 0;
    uint32_t yields = 0;
    
    while (true) {
        uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
        
        if (current == 0) {
            uint32_t next = RWLOCK_WRITER_MASK;
            if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                        memory_order_acquire,
                                                        memory_order_relaxed)) {
                atomic_fetch_sub_explicit(&latch->writer_wait_count, 1, memory_order_release);
                return;
            }
            continue;
        }
        
        if (spins < RWLOCK_MAX_SPIN_COUNT) {
            rwlock_cpu_relax();
            spins++;
        } else if (yields < RWLOCK_MAX_YIELD_COUNT) {
#if defined(__linux__) || defined(__APPLE__)
            sched_yield();
#else
            rwlock_cpu_relax();
#endif
            yields++;
        } else {
#if defined(__linux__)
            uint32_t expected = atomic_load_explicit(&latch->state, memory_order_relaxed);
            if (expected != 0) {
                rwlock_futex_wait(&latch->state, expected);
            }
#elif defined(__APPLE__)
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
#else
            rwlock_cpu_relax();
#endif
            spins = 0;
            yields = 0;
        }
    }
}

void rwlock_unlock(rwlock_t *latch) {
    if (!latch) return;
    
    uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
    
    if (current & RWLOCK_WRITER_MASK) {
        // Releasing Writer
        if (!atomic_compare_exchange_strong_explicit(&latch->state, &current, 0,
                                                       memory_order_release,
                                                       memory_order_relaxed)) {
            // State mutation race detected - invalid unlock sequence
            return;
        }
        
#if defined(__linux__)
        if (atomic_load_explicit(&latch->writer_wait_count, memory_order_relaxed) > 0) {
            rwlock_futex_wake_one(&latch->state);
        } else {
            rwlock_futex_wake_all(&latch->state);
        }
#endif
    } else {
        // Releasing Reader
        while (true) {
            uint32_t readers = current & RWLOCK_READER_MASK;
            if (readers == 0) {
                // Underflow or invalid state
                return;
            }
            uint32_t next = current - RWLOCK_READER_INCR;
            if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                        memory_order_release,
                                                        memory_order_relaxed)) {
                if (next == 0) {
#if defined(__linux__)
                    if (atomic_load_explicit(&latch->writer_wait_count, memory_order_relaxed) > 0) {
                        rwlock_futex_wake_one(&latch->state);
                    }
#endif
                }
                break;
            }
        }
    }
}

void rwlock_upgrade(rwlock_t *latch) {
    if (!latch) return;
    
    // Step 1: Drop reader count by one
    while (true) {
        uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
        uint32_t next = current - RWLOCK_READER_INCR;
        if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                    memory_order_release,
                                                    memory_order_relaxed)) {
            break;
        }
    }
    
    // Step 2: Acquire writer lock (similar to wrlock path with wait counting)
    atomic_fetch_add_explicit(&latch->writer_wait_count, 1, memory_order_acquire);
    
    uint32_t spins = 0;
    uint32_t yields = 0;
    
    while (true) {
        uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
        
        if (current == 0) {
            uint32_t next = RWLOCK_WRITER_MASK;
            if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                        memory_order_acquire,
                                                        memory_order_relaxed)) {
                atomic_fetch_sub_explicit(&latch->writer_wait_count, 1, memory_order_release);
                return;
            }
            continue;
        }
        
        if (spins < RWLOCK_MAX_SPIN_COUNT) {
            rwlock_cpu_relax();
            spins++;
        } else if (yields < RWLOCK_MAX_YIELD_COUNT) {
#if defined(__linux__) || defined(__APPLE__)
            sched_yield();
#else
            rwlock_cpu_relax();
#endif
            yields++;
        } else {
#if defined(__linux__)
            uint32_t expected = atomic_load_explicit(&latch->state, memory_order_relaxed);
            if (expected != 0) {
                rwlock_futex_wait(&latch->state, expected);
            }
#elif defined(__APPLE__)
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
#else
            rwlock_cpu_relax();
#endif
            spins = 0;
            yields = 0;
        }
    }
}

void rwlock_downgrade(rwlock_t *latch) {
    if (!latch) return;
    
    while (true) {
        uint32_t current = atomic_load_explicit(&latch->state, memory_order_relaxed);
        if (!(current & RWLOCK_WRITER_MASK)) {
            // Not holding an exclusive writer lock
            return;
        }
        
        uint32_t next = RWLOCK_READER_INCR; // Transition from Exclusive Writer to single Reader
        if (atomic_compare_exchange_weak_explicit(&latch->state, &current, next,
                                                    memory_order_release,
                                                    memory_order_relaxed)) {
#if defined(__linux__)
            if (atomic_load_explicit(&latch->writer_wait_count, memory_order_relaxed) == 0) {
                rwlock_futex_wake_all(&latch->state);
            }
#endif
            break;
        }
    }
}