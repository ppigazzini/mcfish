#include "search_threads.h"

#include "../../platform/numa.h"
#include "../board/score.h"
#include "../state/worker_construct.h"
#include "pool_source.h"

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

ThreadPool *search_threads_pool(void) {
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
// the same order, ThreadPool::set clears it (thread.cpp:11) and assigns it further down
// (thread.cpp:37) -- so the builder cannot read it back while it is running. Compute the
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
}

bool search_threads_set(size_t count) {
    if (count == 0)
        count = 1;
    if (count > SEARCH_THREADS_MAX)
        count = SEARCH_THREADS_MAX;

    ThreadPool *const pool = search_threads_pool();
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
    return true;
}

size_t search_threads_count(void) { return WorkerCount; }

SearchWorker *search_threads_at(size_t index) {
    return index < WorkerCount ? Workers[index] : nullptr;
}

SearchWorker *search_threads_main(void) {
    if (WorkerCount == 0 && !search_threads_set(1))
        return nullptr;
    return search_threads_at(0);
}

void search_threads_clear(void) {
    for (size_t i = 0; i < WorkerCount; ++i)
        worker_clear(Workers[i]);
}

void search_threads_shutdown(void) {
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

void search_threads_start_siblings(ThreadJobFn job) {
    thread_pool_start_jobs(search_threads_pool(), job, 1);
}

void search_threads_wait_siblings(void) { thread_pool_wait_from(search_threads_pool(), 1); }

// ---- the NumaPolicy dispatcher -------------------------------------------

bool search_threads_set_numa_policy(const char *policy) {
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

// ---- the thread vote -----------------------------------------------------

// Return the vote M has collected: every worker whose best move is M contributes
// `score - min_score + 14`. Upstream keeps this in a hash map; with at most
// SEARCH_THREADS_MAX workers a scan over the same set is the same arithmetic in the same
// order, which is what matters -- the map's iteration order never reaches the result.
static int64_t vote_for(Move m, int32_t min_score) {
    int64_t total = 0;
    for (size_t i = 0; i < WorkerCount; ++i) {
        const RootMove *const rm = &Workers[i]->ctx.root_moves[0];
        if (rm->pv.moves[0] == m)
            total += (int64_t) rm->score - min_score + 14;
    }
    return total;
}

static bool is_decisive_exact(const RootMove *rm) {
    return rm->score != -VALUE_INFINITE && value_is_decisive((Value) rm->score)
        && !root_move_score_is_bound(rm);
}

SearchWorker *search_threads_best(void) {
    if (WorkerCount == 0)
        return nullptr;

    SearchWorker *best = Workers[0];
    if (WorkerCount == 1 || best->ctx.root_moves == nullptr)
        return best;

    int32_t min_score = VALUE_INFINITE;
    for (size_t i = 0; i < WorkerCount; ++i) {
        const int32_t s = Workers[i]->ctx.root_moves[0].score;
        if (s < min_score)
            min_score = s;
    }

    for (size_t i = 0; i < WorkerCount; ++i) {
        const RootMove *const best_rm = &best->ctx.root_moves[0];
        const RootMove *const new_rm = &Workers[i]->ctx.root_moves[0];

        const int64_t best_vote = vote_for(best_rm->pv.moves[0], min_score);
        const int64_t new_vote = vote_for(new_rm->pv.moves[0], min_score);

        // An aborted depth-1 search can leave an INEXACT win or loss score, which is why
        // the decisive test also demands the score not be a bound.
        const bool best_decisive = is_decisive_exact(best_rm);
        const bool new_decisive = is_decisive_exact(new_rm);

        if (best_decisive) {
            // Pick the shortest mate / tablebase conversion.
            if (new_decisive && llabs(new_rm->score) > llabs(best_rm->score))
                best = Workers[i];
        } else if (new_decisive
                   || (!value_is_loss((Value) new_rm->score)
                       && (new_vote > best_vote
                           || (new_vote == best_vote && new_rm->pv.length > best_rm->pv.length)))) {
            best = Workers[i];
        }
    }
    return best;
}
