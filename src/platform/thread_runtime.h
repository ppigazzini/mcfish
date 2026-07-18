// Own the blocking and atomic primitives the thread pool is built from.
//
// Every predicate this file guards is re-checked by its caller in a loop, so a spurious
// wakeup is harmless and a missed wakeup is not: signal and broadcast must be reached on
// every path that changes a predicate. Nothing here allocates, and nothing here blocks
// except condition_wait, so a caller holding a Mutex may safely hold it across any other
// call in this header.
//
// zfish hand-rolls a Drepper futex mutex and a sequence-counter condition variable
// because Zig 0.16 removed std.Thread.Mutex/Condition (zfish
// src/platform/thread_runtime.zig:8). C has no such gap: pthread supplies exactly the
// std::mutex / std::condition_variable pair upstream uses, so wrap those instead of
// re-deriving the futex protocol.
//
// Upstream: thread.h:60 (mutex, cv), thread.h:105 (the pool's stop flag).
// Port source: zfish src/platform/thread_runtime.zig.

#ifndef CCFISH_THREAD_RUNTIME_H
#define CCFISH_THREAD_RUNTIME_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t handle;
} Mutex;

typedef struct {
    pthread_cond_t handle;
} Condition;

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void condition_init(Condition *cv);
void condition_destroy(Condition *cv);

// Release M, block until signalled, then re-acquire M. The caller must hold M, and must
// re-test its predicate on return: a wakeup here carries no promise about the predicate.
void condition_wait(Condition *cv, Mutex *m);

// Wake one waiter / every waiter. Safe to call with or without the mutex held.
void condition_signal(Condition *cv);
void condition_broadcast(Condition *cv);

// Wrap the two atomics the pool shares with the search. Both use relaxed ordering, which
// is what zfish's `.monotonic` means: these are flags and counters read outside any
// happens-before chain, never a handoff of other memory.
typedef struct {
    atomic_bool value;
} AtomicBool;

typedef struct {
    atomic_uint_least64_t value;
} AtomicU64;

void atomic_bool_init(AtomicBool *a, bool value);
void atomic_bool_store(AtomicBool *a, bool value);
bool atomic_bool_load(const AtomicBool *a);

void atomic_u64_init(AtomicU64 *a, uint64_t value);
void atomic_u64_store(AtomicU64 *a, uint64_t value);
uint64_t atomic_u64_load(const AtomicU64 *a);

// Add DELTA and return the value held BEFORE the addition, wrapping on overflow as
// unsigned arithmetic does.
uint64_t atomic_u64_fetch_add(AtomicU64 *a, uint64_t delta);

#endif  // CCFISH_THREAD_RUNTIME_H
