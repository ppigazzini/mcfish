#include "worker_pool.h"

#include "numa.h"
#include "../engine/state/worker_construct.h"
#include "../engine/search/pool_source.h"
#include "../engine/search/tt.h"
#include "../engine/search/worker_set.h"

#include <stdlib.h>
#include <string.h>

// Bound the worker set at upstream's advertised Threads maximum, so a policy string or a
// mis-parsed option cannot ask for an allocation this module then has to unwind.
enum { SEARCH_THREADS_MAX = 1024 };

static ThreadPool Pool;
static bool PoolReady = false;

static NumaReplicationContext Numa;
static bool NumaReady = false;
static NumaPolicyMode Policy = NUMA_POLICY_AUTO;

// Hold one shared history bank per occupied NUMA node. `banks[i]` is null for a node with
// no worker on it, which is every node but 0 on a single-node run.
static SharedHistories **Banks = nullptr;
static size_t BankCount = 0;

// Hold every worker in pool order, so the vote and the counter sums are one loop rather
// than a walk through the thread module.
static SearchWorker **Workers = nullptr;
static size_t WorkerCount = 0;

// ---- the pool ------------------------------------------------------------

static void numa_ensure(void) {
    if (NumaReady)
        return;
    NumaConfig cfg;
    numa_config_init(&cfg);
    numa_context_init(&Numa, &cfg);
    // Read the real topology once. `auto` still refuses to bind a single thread, so this
    // does not make a one-thread run touch an affinity mask.
    numa_context_set_system(&Numa);
    NumaReady = true;
}

// Render the topology the pool binds under, for `Available processors`. The context
// is this module's, so the reporter has to come from here.
char *worker_pool_numa_config_string(void) {
    numa_ensure();
    return numa_config_to_string(numa_context_config(&Numa));
}

static ThreadPool *pool_handle(void) {
    if (!PoolReady) {
        thread_pool_init(&Pool);
        PoolReady = true;
    }
    return &Pool;
}

// ---- the shared history banks -------------------------------------------

static void banks_free(void) {
    for (size_t i = 0; i < BankCount; ++i)
        shared_histories_destroy(Banks[i]);
    free(Banks);
    Banks = nullptr;
    BankCount = 0;
}

static void hook_clear_histories(void *ctx) {
    (void) ctx;
    banks_free();
}

// Insert one node's bank, sized for COUNT threads. COUNT arrives rounded up to a power of
// two, which is what makes the table index a mask.
static bool hook_insert_history(void *ctx, size_t node_index, size_t count, bool bound) {
    (void) ctx;
    (void) bound;

    if (node_index >= BankCount) {
        SharedHistories **grown = realloc(Banks, (node_index + 1) * sizeof *grown);
        if (grown == nullptr)
            return false;
        for (size_t i = BankCount; i <= node_index; ++i)
            grown[i] = nullptr;
        Banks = grown;
        BankCount = node_index + 1;
    }

    shared_histories_destroy(Banks[node_index]);
    Banks[node_index] = shared_histories_create(count);
    return Banks[node_index] != nullptr;
}

// ---- worker construction -------------------------------------------------

// Hold the thread -> node map and the per-node totals the builder needs.
//
// The pool publishes its bound vector only AFTER thread_pool_set returns -- upstream has
// the same order, ThreadPool::set clears it (thread.cpp:162) and assigns it further down
// (thread.cpp:188) -- so the builder cannot read it back while it is running. Compute the
// same distribution here first, from the same inputs and the same functions, and hand it
// to the builder. Both are deterministic over the topology, so the two agree by
// construction rather than by luck.
static size_t NodeOfThread[SEARCH_THREADS_MAX];
static size_t PerNodePlaced[SEARCH_THREADS_MAX];
static size_t PerNodeTotal[SEARCH_THREADS_MAX];

static bool builder_build(void *ctx, size_t idx, Thread *thread) {
    (void) ctx;

    const size_t node = idx < SEARCH_THREADS_MAX ? NodeOfThread[idx] : 0;
    SharedHistories *const bank = node < BankCount ? Banks[node] : nullptr;
    if (bank == nullptr)
        return false;

    const WorkerCtorInputs in = {
        .shared_history = bank,
        .threads = &Pool,
        .thread_idx = idx,
        .numa_thread_idx = PerNodePlaced[node]++,
        .numa_total = PerNodeTotal[node] != 0 ? PerNodeTotal[node] : 1,
        .numa_access_token = node,
    };

    // Build ON this thread: the pool has already bound it, so the block -- megabytes of
    // history tables -- is first-touched on the node that will read it.
    SearchWorker *const w = worker_create(&in);
    if (w == nullptr)
        return false;

    thread_set_worker(thread, w);
    Workers[idx] = w;
    return true;
}

static void builder_destroy(void *worker) { worker_destroy((SearchWorker *) worker); }

// ---- the counter seam ----------------------------------------------------

static uint64_t pool_nodes_sum(void *ctx) {
    (void) ctx;
    uint64_t total = 0;
    for (size_t i = 0; i < WorkerCount; ++i)
        total += ctx_nodes(&Workers[i]->ctx);
    return total;
}

static uint64_t pool_tb_hits_sum(void *ctx) {
    (void) ctx;
    uint64_t total = 0;
    for (size_t i = 0; i < WorkerCount; ++i)
        total += ctx_tb_hits(&Workers[i]->ctx);
    return total;
}

// Sum AND reset, which is one operation upstream (search.cpp:563-564): a sum that does
// not reset double-counts the next iteration's instability.
static uint64_t pool_collect_bmc(void *ctx) {
    (void) ctx;
    uint64_t total = 0;
    for (size_t i = 0; i < WorkerCount; ++i) {
        total += ctx_best_move_changes(&Workers[i]->ctx);
        ctx_set_best_move_changes(&Workers[i]->ctx, 0);
    }
    return total;
}

static void install_counter_seam(void) {
    PoolCounters.ctx = nullptr;
    PoolCounters.nodes = pool_nodes_sum;
    PoolCounters.tb_hits = pool_tb_hits_sum;
    PoolCounters.collect_best_move_changes = pool_collect_bmc;
}

// ---- TT-clear seam -------------------------------------------------------
//
// Hand tt_clear the pool so it can zero one span per thread. Single-node dispatch
// order is plain thread order; upstream's NUMA-sorted dispatch (tt.cpp:187) reduces
// to the same order with one node.

static size_t tt_clear_thread_count(void *ctx) {
    (void) ctx;
    return WorkerCount;
}

static void
tt_clear_run_on_thread(void *ctx, size_t index, void (*job)(void *job_ctx), void *job_ctx) {
    (void) ctx;
    thread_pool_run_on_thread(pool_handle(), index, job, job_ctx);
}

static void tt_clear_wait_all(void *ctx) {
    (void) ctx;
    thread_pool_wait_from(pool_handle(), 0);
}

static void install_tt_clear_seam(void) {
    TTClearPool.ctx = nullptr;
    TTClearPool.thread_count = tt_clear_thread_count;
    TTClearPool.run_on_thread = tt_clear_run_on_thread;
    TTClearPool.wait_all = tt_clear_wait_all;
}

// ---- lifecycle -----------------------------------------------------------

static void workers_release(void) {
    // The pool's teardown calls builder_destroy on every attached block, so this only
    // drops the index. Clearing it first keeps the counter seam from walking freed
    // workers while the pool unwinds.
    WorkerCount = 0;
    free(Workers);
    Workers = nullptr;

    PoolCounters.nodes = nullptr;
    PoolCounters.tb_hits = nullptr;
    PoolCounters.collect_best_move_changes = nullptr;

    TTClearPool.thread_count = nullptr;
    TTClearPool.run_on_thread = nullptr;
    TTClearPool.wait_all = nullptr;
}

static bool pool_resize_impl(void *ctx, size_t count) {
    (void) ctx;
    if (count == 0)
        count = 1;
    if (count > SEARCH_THREADS_MAX)
        count = SEARCH_THREADS_MAX;

    ThreadPool *const pool = pool_handle();
    numa_ensure();

    workers_release();
    thread_pool_clear(pool);

    SearchWorker **const slots = calloc(count, sizeof *slots);
    if (slots == nullptr)
        return false;
    Workers = slots;

    memset(NodeOfThread, 0, sizeof NodeOfThread);
    memset(PerNodePlaced, 0, sizeof PerNodePlaced);
    memset(PerNodeTotal, 0, sizeof PerNodeTotal);

    // Mirror thread_pool_reconfigure's bind decision exactly. A single thread never binds
    // under `auto`, which is what keeps the one-thread path identical on every host.
    bool do_bind;
    switch (Policy) {
    case NUMA_POLICY_NONE :
        do_bind = false;
        break;
    case NUMA_POLICY_AUTO :
        do_bind = numa_suggests_binding_threads(&Numa, count);
        break;
    default :
        do_bind = true;
        break;
    }

    if (do_bind)
        (void) numa_distribute_threads_among_nodes(&Numa, count, NodeOfThread);
    for (size_t i = 0; i < count; ++i) {
        if (NodeOfThread[i] >= SEARCH_THREADS_MAX)
            NodeOfThread[i] = 0;
        PerNodeTotal[NodeOfThread[i]] += 1;
    }

    const ThreadBuilder builder = { .ctx = nullptr,
                                    .build = builder_build,
                                    .destroy = builder_destroy };
    const SharedHistoryHooks hooks = { .ctx = nullptr,
                                       .clear_histories = hook_clear_histories,
                                       .insert_history = hook_insert_history };

    if (!thread_pool_reconfigure(pool, &Numa, count, Policy, &builder, &hooks)) {
        workers_release();
        thread_pool_clear(pool);
        return false;
    }

    WorkerCount = count;
    install_counter_seam();
    install_tt_clear_seam();
    return true;
}

static size_t pool_count_impl(void *ctx) {
    (void) ctx;
    return WorkerCount;
}

static SearchWorker *pool_at_impl(void *ctx, size_t index) {
    (void) ctx;
    return index < WorkerCount ? Workers[index] : nullptr;
}


static void pool_clear_impl(void *ctx) {
    (void) ctx;
    for (size_t i = 0; i < WorkerCount; ++i)
        worker_clear(Workers[i]);
}

static void pool_shutdown_impl(void *ctx) {
    (void) ctx;
    workers_release();
    if (PoolReady) {
        thread_pool_clear(&Pool);
        PoolReady = false;
    }
    banks_free();
    if (NumaReady) {
        numa_context_destroy(&Numa);
        NumaReady = false;
    }
}

static void pool_start_siblings_impl(void *ctx, WorkerJobFn job) {
    (void) ctx;
    thread_pool_start_jobs(pool_handle(), job, 1);
}

static void pool_wait_siblings_impl(void *ctx) {
    (void) ctx;
    thread_pool_wait_from(pool_handle(), 1);
}

static void pool_run_main_impl(void *ctx, WorkerJobFn job, void *job_ctx) {
    (void) ctx;
    thread_pool_run_on_thread(pool_handle(), 0, job, job_ctx);
}

static void pool_wait_main_impl(void *ctx) {
    (void) ctx;
    thread_pool_wait_on_thread(pool_handle(), 0);
}

static void pool_set_stop_impl(void *ctx, bool value) {
    (void) ctx;
    thread_pool_set_stop(pool_handle(), value);
}

static atomic_bool *pool_stop_flag_impl(void *ctx) {
    (void) ctx;
    return &pool_handle()->stop.value;
}

static void pool_set_increase_depth_impl(void *ctx, bool value) {
    (void) ctx;
    thread_pool_set_increase_depth(pool_handle(), value);
}

static atomic_bool *pool_increase_depth_flag_impl(void *ctx) {
    (void) ctx;
    return &pool_handle()->increase_depth.value;
}

// ---- the NumaPolicy dispatcher -------------------------------------------

static bool pool_numa_policy_impl(void *ctx, const char *policy) {
    (void) ctx;
    numa_ensure();

    if (policy == nullptr || strcmp(policy, "auto") == 0) {
        numa_context_set_system(&Numa);
        Policy = NUMA_POLICY_AUTO;
        return true;
    }
    if (strcmp(policy, "none") == 0) {
        numa_context_set_none(&Numa);
        Policy = NUMA_POLICY_NONE;
        return true;
    }
    if (strcmp(policy, "system") == 0) {
        numa_context_set_system(&Numa);
        Policy = NUMA_POLICY_EXPLICIT;
        return true;
    }
    if (strcmp(policy, "hardware") == 0) {
        numa_context_set_hardware(&Numa);
        Policy = NUMA_POLICY_EXPLICIT;
        return true;
    }

    // Anything else is an explicit topology, which always binds (upstream numa.h:768).
    // A string naming no node at all is REFUSED rather than degraded: a config with zero
    // nodes makes every distribution and binding decision divide by zero.
    if (!numa_context_set_from_string(&Numa, policy, strlen(policy)))
        return false;
    Policy = NUMA_POLICY_EXPLICIT;
    return true;
}

// Hand the whole set to the engine. Called once by the composition root, before any
// search: the engine runs on its built-in one-worker set until this lands.
void worker_pool_install(void) {
    WorkerSet = (WorkerSetOps) { .ctx = nullptr,
                                 .resize = pool_resize_impl,
                                 .set_numa_policy = pool_numa_policy_impl,
                                 .count = pool_count_impl,
                                 .at = pool_at_impl,
                                 .clear = pool_clear_impl,
                                 .shutdown = pool_shutdown_impl,
                                 .run_main = pool_run_main_impl,
                                 .wait_main = pool_wait_main_impl,
                                 .start_siblings = pool_start_siblings_impl,
                                 .wait_siblings = pool_wait_siblings_impl,
                                 .set_stop = pool_set_stop_impl,
                                 .stop_flag = pool_stop_flag_impl,
                                 .set_increase_depth = pool_set_increase_depth_impl,
                                 .increase_depth_flag = pool_increase_depth_flag_impl };
}
