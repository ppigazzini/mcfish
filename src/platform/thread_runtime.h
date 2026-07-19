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

// Wrap the two atomics the pool shares with the search.
//
// AtomicBool is SEQUENTIALLY CONSISTENT, because upstream's `stop`, `increaseDepth` and
// `ponder` are plain `std::atomic_bool` assignments and reads (thread.h:157) and only two
// sites in the whole engine opt out -- the two in-tree abort checks, search.cpp:770 and
// search.cpp:1403, which say `memory_order_relaxed` explicitly. Making every access
// relaxed is not a free optimisation: `stop` is raised by one thread and must be seen by
// the ID loop of every other, and under relaxed ordering the compiler may hoist the load
// out of the depth loop entirely, so a `stop` arrives only when some unrelated barrier
// happens to publish it. Use the _relaxed accessors ONLY at the two per-node checks, where
// upstream does, and where a one-node-late abort is what the ordering buys back.
typedef struct {
    atomic_bool value;
} AtomicBool;

typedef struct {
    atomic_uint_least64_t value;
} AtomicU64;

void atomic_bool_init(AtomicBool *a, bool value);
void atomic_bool_store(AtomicBool *a, bool value);
bool atomic_bool_load(const AtomicBool *a);

// Read without ordering. Reserved for the two in-tree abort checks named above; anywhere
// else this silently converts "stop the search now" into "stop it eventually".
bool atomic_bool_load_relaxed(const AtomicBool *a);

void atomic_u64_init(AtomicU64 *a, uint64_t value);
void atomic_u64_store(AtomicU64 *a, uint64_t value);
uint64_t atomic_u64_load(const AtomicU64 *a);

// Add DELTA and return the value held BEFORE the addition, wrapping on overflow as
// unsigned arithmetic does.
uint64_t atomic_u64_fetch_add(AtomicU64 *a, uint64_t delta);

#endif  // CCFISH_THREAD_RUNTIME_H
