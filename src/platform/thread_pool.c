#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>

size_t next_power_of_two(uint64_t count) {
    if (count <= 1)
        return 1;

    // Count leading zeros on the 64-bit value explicitly: promoting through `int` here
    // would silently truncate a thread count above 2^31 on a host that reports one.
    return (size_t) 2 << (63 - __builtin_clzll(count - 1));
}

void thread_pool_init(ThreadPool *pool) {
    memset(pool, 0, sizeof *pool);
    atomic_bool_init(&pool->stop, false);
    atomic_bool_init(&pool->increase_depth, false);
}

// Bind one thread to its node from inside that thread, so the affinity is set before the
// builder first-touches the Worker's pages.
typedef struct {
    const NumaReplicationContext *numa_ctx;
    size_t node;
} BindJob;

static void bind_job(void *ctx) {
    const BindJob *job = (const BindJob *) ctx;
    // Ignore the result: a host that refuses the binding leaves the thread on its
    // inherited affinity, which is slower on a multi-node box and not incorrect.
    (void) numa_config_bind_current_thread(numa_context_config(job->numa_ctx), job->node);
}

// Carry one shared-history insert to whichever thread runs it, so the same call works
// bound (on a throwaway thread pinned to the node) and unbound (inline).
typedef struct {
    const SharedHistoryHooks *hooks;
    size_t node;
    size_t count;
    bool bound;
    bool ok;
} InsertHistoryJob;

static void insert_history_job(void *ctx) {
    InsertHistoryJob *job = (InsertHistoryJob *) ctx;
    job->ok = job->hooks->insert_history(job->hooks->ctx, job->node, job->count, job->bound);
}

static void destroy_thread(const ThreadBuilder *builder, Thread *thread) {
    // Join the idle loop FIRST -- no thread touches `worker` after this -- then hand the
    // attached block back to whoever built it.
    thread_join(thread);

    void *worker = thread_worker(thread);
    if (worker != nullptr && builder->destroy != nullptr)
        builder->destroy(worker);
    thread_set_worker(thread, nullptr);

    free(thread);
}

void thread_pool_clear(ThreadPool *pool) {
    if (pool->threads != nullptr) {
        // Drain any queued or in-flight job BEFORE tearing threads down, as upstream does
        // before deleting threads. The teardown path runs with the stop flag already set,
        // so an in-flight search bails immediately and emits its bestmove here.
        for (size_t i = 0; i < pool->thread_count; ++i)
            thread_wait_for_search_finished(pool->threads[i]);

        for (size_t i = 0; i < pool->thread_count; ++i)
            destroy_thread(&pool->builder, pool->threads[i]);

        free(pool->threads);
    }

    pool->threads = nullptr;
    pool->thread_count = 0;

    // Free the bound vector here too: thread_pool_bound_nodes_assign is the only other
    // free site, and clear() is the one point both reconfigure and teardown pass through.
    free(pool->bound_nodes);
    pool->bound_nodes = nullptr;
    pool->bound_count = 0;
}

bool thread_pool_set(ThreadPool *pool,
                     size_t count,
                     const ThreadBuilder *builder,
                     const NumaReplicationContext *numa_ctx,
                     const size_t *bind_nodes) {
    thread_pool_clear(pool);
    pool->builder = *builder;

    // Clear stop / increaseDepth at the start; no search can be in flight here.
    atomic_bool_store(&pool->stop, false);
    atomic_bool_store(&pool->increase_depth, false);

    if (count == 0)
        return true;

    Thread **threads = calloc(count, sizeof *threads);
    if (threads == nullptr)
        return false;

    size_t built = 0;
    for (; built < count; ++built) {
        Thread *thread = calloc(1, sizeof *thread);
        if (thread == nullptr)
            break;

        if (!thread_spawn(thread, built)) {
            free(thread);
            break;
        }
        threads[built] = thread;

        if (numa_ctx != nullptr && bind_nodes != nullptr) {
            BindJob job = { numa_ctx, bind_nodes[built] };
            thread_start_job(thread, bind_job, &job);
            thread_wait_for_search_finished(thread);  // keep `job` alive until it is read
        }

        if (!builder->build(builder->ctx, built, thread)) {
            ++built;  // include this thread in the unwind: it is spawned and running
            break;
        }
    }

    if (built != count) {
        for (size_t i = 0; i < built; ++i) {
            if (threads[i] != nullptr)
                destroy_thread(builder, threads[i]);
        }
        free(threads);
        return false;
    }

    pool->threads = threads;
    pool->thread_count = count;
    return true;
}

size_t thread_pool_num_threads(const ThreadPool *pool) { return pool->thread_count; }

Thread *thread_pool_thread_at(const ThreadPool *pool, size_t index) {
    return index < pool->thread_count ? pool->threads[index] : nullptr;
}

void thread_pool_run_on_thread(ThreadPool *pool, size_t index, ThreadJobFn job_fn, void *job_ctx) {
    Thread *thread = thread_pool_thread_at(pool, index);
    if (thread != nullptr)
        thread_start_job(thread, job_fn, job_ctx);
}

void thread_pool_wait_on_thread(ThreadPool *pool, size_t index) {
    Thread *thread = thread_pool_thread_at(pool, index);
    if (thread != nullptr)
        thread_wait_for_search_finished(thread);
}

void thread_pool_start_jobs(ThreadPool *pool, ThreadJobFn job_fn, size_t first) {
    for (size_t i = first; i < pool->thread_count; ++i)
        thread_start_job(pool->threads[i], job_fn, thread_worker(pool->threads[i]));
}

void thread_pool_wait_from(ThreadPool *pool, size_t first) {
    for (size_t i = first; i < pool->thread_count; ++i)
        thread_wait_for_search_finished(pool->threads[i]);
}

void thread_pool_set_stop(ThreadPool *pool, bool value) { atomic_bool_store(&pool->stop, value); }

bool thread_pool_stopped(const ThreadPool *pool) { return atomic_bool_load(&pool->stop); }

void thread_pool_set_increase_depth(ThreadPool *pool, bool value) {
    atomic_bool_store(&pool->increase_depth, value);
}

bool thread_pool_increase_depth(const ThreadPool *pool) {
    return atomic_bool_load(&pool->increase_depth);
}

bool thread_pool_bound_nodes_assign(ThreadPool *pool, const size_t *nodes, size_t count) {
    free(pool->bound_nodes);
    pool->bound_nodes = nullptr;
    pool->bound_count = 0;

    if (nodes == nullptr || count == 0)
        return true;

    size_t *buf = malloc(count * sizeof *buf);
    if (buf == nullptr)
        return false;

    memcpy(buf, nodes, count * sizeof *buf);
    pool->bound_nodes = buf;
    pool->bound_count = count;
    return true;
}

size_t thread_pool_bound_count(const ThreadPool *pool) { return pool->bound_count; }

size_t thread_pool_bound_at(const ThreadPool *pool, size_t index) {
    return index < pool->bound_count ? pool->bound_nodes[index] : 0;
}

bool thread_pool_reconfigure(ThreadPool *pool,
                             const NumaReplicationContext *numa_ctx,
                             size_t requested,
                             NumaPolicyMode policy,
                             const ThreadBuilder *builder,
                             const SharedHistoryHooks *histories) {
    if (pool->thread_count > 0) {
        thread_pool_wait_on_thread(pool, 0);
        thread_pool_clear(pool);
    }

    if (requested == 0)
        return true;

    bool do_bind;
    switch (policy) {
    case NUMA_POLICY_NONE :
        do_bind = false;
        break;
    case NUMA_POLICY_AUTO :
        do_bind = numa_suggests_binding_threads(numa_ctx, requested);
        break;
    default :
        do_bind = true;
        break;
    }

    size_t *bound_nodes = calloc(requested, sizeof *bound_nodes);
    if (bound_nodes == nullptr)
        return false;

    if (do_bind)
        (void) numa_distribute_threads_among_nodes(numa_ctx, requested, bound_nodes);

    const size_t ctx_nodes = numa_context_node_count(numa_ctx);
    const size_t node_count = ctx_nodes > 1 ? ctx_nodes : 1;

    size_t *threads_per_node = calloc(node_count, sizeof *threads_per_node);
    if (threads_per_node == nullptr) {
        free(bound_nodes);
        return false;
    }

    if (do_bind) {
        for (size_t i = 0; i < requested; ++i)
            threads_per_node[bound_nodes[i]] += 1;
    } else {
        threads_per_node[0] = requested;
    }

    if (histories != nullptr && histories->clear_histories != nullptr)
        histories->clear_histories(histories->ctx);

    for (size_t node = 0; node < node_count; ++node) {
        const size_t count = threads_per_node[node];
        if (count == 0)
            continue;
        if (histories == nullptr || histories->insert_history == nullptr)
            continue;

        InsertHistoryJob job = { histories, node, next_power_of_two(count), do_bind, false };

        // Build the bank ON the node that will read it. The bank is tens of megabytes and
        // its pages are first-touched by whoever writes them first, so inserting every
        // node's bank from the calling thread puts them all on ONE node -- which is
        // precisely the cost per-node banks exist to avoid. Upstream routes the same
        // insert through execute_on_numa_node (thread.cpp:208).
        if (do_bind)
            numa_execute_on_node(numa_ctx, node, insert_history_job, &job);
        else
            insert_history_job(&job);

        if (!job.ok) {
            free(threads_per_node);
            free(bound_nodes);
            return false;
        }
    }

    free(threads_per_node);

    if (!thread_pool_set(pool, requested, builder, numa_ctx, do_bind ? bound_nodes : nullptr)) {
        free(bound_nodes);
        return false;
    }

    // Record the bound footprint AFTER set(): set() clears it, so assigning before was
    // silently undone. Upstream has the same order within one function -- ThreadPool::set
    // clears boundThreadToNumaNode (thread.cpp:11) and assigns it further down
    // (thread.cpp:37).
    const bool ok = thread_pool_bound_nodes_assign(pool, do_bind ? bound_nodes : nullptr,
                                                   do_bind ? requested : 0);
    free(bound_nodes);

    if (!ok) {
        thread_pool_clear(pool);
        return false;
    }

    thread_pool_wait_on_thread(pool, 0);
    return true;
}
