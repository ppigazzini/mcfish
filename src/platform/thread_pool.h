// Own the Lazy-SMP worker pool: the thread vector, the shared stop flag, and the NUMA
// binding plan that decides which node each thread lives on.
//
// The pool is INERT until thread_pool_set is called, and a pool of one thread performs no
// binding, no distribution and no cross-thread synchronisation beyond the single idle-loop
// handshake. That is the property the `signature` gate rests on: a one-thread search must
// visit the same nodes in the same order every run, so nothing here may make thread 0's
// behaviour depend on wall-clock time, on scheduling, or on an address.
//
// The Worker is attached through an injected ThreadBuilder rather than constructed here,
// so the pool's bookkeeping is exercised without the engine graph and the pool never
// reaches into the search zone. The pool owns the Thread objects; the builder owns
// whatever it hangs off each thread's `worker` slot and takes it back through `destroy`.
//
// Upstream: thread.cpp:152 (ThreadPool::set clears boundThreadToNumaNode at :162),
// thread.cpp:188 (and assigns it further down), thread.cpp:254 (clear), thread.cpp:406
// (start_searching).

#ifndef MCFISH_THREAD_POOL_H
#define MCFISH_THREAD_POOL_H

#include "numa.h"
#include "thread.h"
#include "thread_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mirror the UCI NumaPolicy option's three shapes. Anything other than `none` and `auto`
// is an explicit topology, which always binds (numa.h:768).
typedef enum {
    NUMA_POLICY_NONE = 0,
    NUMA_POLICY_AUTO = 1,
    NUMA_POLICY_EXPLICIT = 2
} NumaPolicyMode;

// Attach the per-thread search payload. Called once per thread, on that thread's own NUMA
// node when binding is in effect, so the Worker's pages are first-touched where they will
// be read. Return false on allocation failure, leaving the thread's `worker` slot null --
// the pool then unwinds every thread built so far, and calls `destroy` only on slots the
// builder actually filled.
typedef struct {
    void *ctx;
    bool (*build)(void *ctx, size_t idx, Thread *thread);
    // Release the block `build` attached. May be null when the builder attaches nothing.
    void (*destroy)(void *worker);
} ThreadBuilder;

// Rebuild the per-NUMA-node shared histories the workers share. Kept as a seam because the
// history tables live in the engine zone, which this module must not name.
typedef struct {
    void *ctx;
    void (*clear_histories)(void *ctx);
    // Insert one history bank for NODE_INDEX sized for COUNT threads (COUNT is already
    // rounded up to a power of two). Return false on allocation failure.
    bool (*insert_history)(void *ctx, size_t node_index, size_t count, bool bound);
} SharedHistoryHooks;

typedef struct {
    Thread **threads;
    size_t thread_count;

    // Hold one NUMA node index per thread when binding is in effect, empty otherwise.
    size_t *bound_nodes;
    size_t bound_count;

    ThreadBuilder builder;

    // Share these two with the search. Sequentially consistent (see thread_runtime.h):
    // they are polled across threads, so a raised flag must be seen without waiting on an
    // unrelated barrier.
    AtomicBool stop;
    AtomicBool increase_depth;
} ThreadPool;

// Round COUNT up to a power of two, with 0 and 1 both mapping to 1. The shared history
// banks are sized this way, so the answer must not depend on the platform's int width.
size_t next_power_of_two(uint64_t count);

void thread_pool_init(ThreadPool *pool);

// Build COUNT threads and attach a Worker to each. Clear any previous pool first, along
// with the bound-node vector -- upstream clears it in set() and assigns it further down
// (thread.cpp:162, thread.cpp:188), so an assignment made before set() is silently undone.
//
// Pass NUMA_CTX and BIND_NODES together to bind: each thread confines itself to
// BIND_NODES[i]'s CPUs before its Worker is built. Pass either as null to leave every
// thread on its inherited affinity. Return false on OOM or a refused thread spawn, leaving
// the pool empty.
bool thread_pool_set(ThreadPool *pool,
                     size_t count,
                     const ThreadBuilder *builder,
                     const NumaReplicationContext *numa_ctx,
                     const size_t *bind_nodes);

// Wait for every in-flight job, join and free every thread, and release the bound vector.
// Idempotent, and safe on a zeroed pool.
void thread_pool_clear(ThreadPool *pool);

size_t thread_pool_num_threads(const ThreadPool *pool);

// Return thread INDEX, or nullptr when INDEX is out of range.
Thread *thread_pool_thread_at(const ThreadPool *pool, size_t index);

void thread_pool_run_on_thread(ThreadPool *pool, size_t index, ThreadJobFn job_fn, void *job_ctx);
void thread_pool_wait_on_thread(ThreadPool *pool, size_t index);

// Submit JOB_FN to every thread from FIRST onward, passing each thread's own `worker` as
// the context. Pass FIRST 1 to start the Lazy-SMP siblings while the caller drives thread
// 0 itself.
void thread_pool_start_jobs(ThreadPool *pool, ThreadJobFn job_fn, size_t first);

// Wait for every thread from FIRST onward.
void thread_pool_wait_from(ThreadPool *pool, size_t first);

void thread_pool_set_stop(ThreadPool *pool, bool value);
bool thread_pool_stopped(const ThreadPool *pool);
void thread_pool_set_increase_depth(ThreadPool *pool, bool value);
bool thread_pool_increase_depth(const ThreadPool *pool);

// Replace the bound-node vector. Free any prior buffer on every reassign; pass null or a
// zero count to clear. Return false on OOM, leaving the vector empty.
bool thread_pool_bound_nodes_assign(ThreadPool *pool, const size_t *nodes, size_t count);

size_t thread_pool_bound_count(const ThreadPool *pool);
size_t thread_pool_bound_at(const ThreadPool *pool, size_t index);

// Rebuild the pool for REQUESTED threads under POLICY: decide whether to bind, distribute
// the threads across nodes, rebuild the shared history banks one per occupied node, then
// build the threads. Return false on OOM or a refused spawn, leaving the pool empty.
//
// REQUESTED == 1 never binds -- numa_config_suggests_binding_threads refuses a single
// thread, and an explicit policy still resolves to the one node it names -- so the
// single-threaded path takes the same shape on every host.
bool thread_pool_reconfigure(ThreadPool *pool,
                             const NumaReplicationContext *numa_ctx,
                             size_t requested,
                             NumaPolicyMode policy,
                             const ThreadBuilder *builder,
                             const SharedHistoryHooks *histories);

#endif  // MCFISH_THREAD_POOL_H
