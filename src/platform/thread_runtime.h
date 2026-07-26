// Own the blocking and atomic primitives the thread pool is built from.
//
// Every predicate this file guards is re-checked by its caller in a loop, so a spurious
// wakeup is harmless and a missed wakeup is not: signal and broadcast must be reached on
// every path that changes a predicate. Nothing here allocates, and nothing here blocks
// except condition_wait, so a caller holding a Mutex may safely hold it across any other
// call in this header.
//
// Wrap pthreads: they supply exactly the std::mutex / std::condition_variable pair
// upstream uses, so there is no reason to re-derive a futex protocol here.
//
// Upstream: thread.h:102-103 (mutex, cv), thread.h:157 (the pool's stop flag).

#ifndef MCFISH_THREAD_RUNTIME_H
#define MCFISH_THREAD_RUNTIME_H

#include <pthread.h>
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

// The atomics the pool shares with the search live in the ENGINE zone -- they are
// C11 and not a platform service, and keeping them here forced the engine to depend
// on this header. Re-exported so platform callers need only this include.
#include "../engine/state/atomic.h"

#endif  // MCFISH_THREAD_RUNTIME_H
