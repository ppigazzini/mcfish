// Own one OS thread and the idle-loop handshake that feeds it work.
//
// A Thread parks in an idle loop from the moment it is spawned and runs exactly one job
// at a time. `searching` is the single predicate: thread_start_job sets it and hands over
// a job, the idle loop clears it and broadcasts once the job returns, and
// thread_wait_for_search_finished blocks until it is clear. The invariant callers depend
// on is that thread_spawn returns only after the thread has reached the parked state, so
// a job submitted immediately afterwards cannot be lost.
//
// `worker` is deliberately opaque here. The pool's builder attaches the per-thread search
// payload; this module never dereferences it, which is what keeps the thread vehicle
// separable from the search zone it will eventually drive.
//
// Upstream: thread.cpp:41 (Thread::Thread), thread.cpp:82 (idle_loop), thread.cpp:60
// (wait_for_search_finished), thread.cpp:71 (run_custom_job).
// Port source: zfish src/platform/search_thread.zig, src/platform/thread_runtime.zig:123.

#ifndef CCFISH_THREAD_H
#define CCFISH_THREAD_H

#include "thread_runtime.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

// Run one unit of work on a Thread. The context is whatever the submitter passed; a null
// context is legal.
typedef void (*ThreadJobFn)(void *ctx);

typedef struct {
    // Hold the per-thread search payload the pool's builder attaches. Opaque: this module
    // only stores and returns it.
    void *worker;
    size_t idx;

    pthread_t handle;
    bool started;

    Mutex mutex;
    Condition cond;

    // Guard these four with `mutex`.
    ThreadJobFn job_fn;
    void *job_ctx;
    bool searching;
    bool exit;
} Thread;

// Spawn T's idle loop and return once it has parked. Return false if the OS refused the
// thread, leaving T safe to discard without a join.
bool thread_spawn(Thread *t, size_t idx);

// Submit a job and return immediately. Wait for any job already in flight first, so
// submissions to one thread never overlap.
void thread_start_job(Thread *t, ThreadJobFn job_fn, void *job_ctx);

// Block until T's current job finishes. Return immediately when T is parked, and when T
// was never spawned.
void thread_wait_for_search_finished(Thread *t);

// Wake the parked idle loop, join it, and release the primitives. Idempotent. Any job
// still in flight runs to completion first -- dropping it would lose the bestmove of a
// search that a `go` had already queued.
void thread_join(Thread *t);

void *thread_worker(const Thread *t);
void thread_set_worker(Thread *t, void *worker);
size_t thread_index(const Thread *t);

// Confine the CALLING thread to the given CPU indices. Return false when the host refuses
// -- a restricted cgroup, a seccomp filter, an index outside the mask -- in which case the
// thread keeps its inherited affinity and stays fully functional. Never treat a false
// return as fatal: an unbound thread is slower on a multi-node host, not incorrect.
bool thread_set_affinity(const size_t *cpus, size_t count);

// Return the number of logical CPUs the process may run on, at least 1.
size_t thread_hardware_concurrency(void);

#endif  // CCFISH_THREAD_H
