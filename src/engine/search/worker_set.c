#include "worker_set.h"

#include "../board/score.h"
#include "../state/atomic.h"
#include "../state/worker_construct.h"
#include "history.h"
#include "pool_source.h"

#include <stdlib.h>

// ---- the built-in one-worker set -----------------------------------------
//
// Everything below the seam, for a host that registers nothing. It builds one worker
// over one history bank and runs every job inline on the calling thread.
//
// That is a real search, not a stub: `search_go` blocks and drives worker 0 inline
// even when a pool is attached, so this path walks the same tree in the same order.
// What it cannot do is spawn a second thread, and `solo_resize` says so by refusing a
// count above one instead of quietly searching with fewer workers than asked for.

static SearchWorker *SoloWorker = nullptr;
static SharedHistories *SoloBank = nullptr;
static AtomicBool SoloStop;
static AtomicBool SoloIncreaseDepth;
static bool SoloFlagsReady = false;

static void solo_flags_ensure(void) {
    if (SoloFlagsReady)
        return;
    atomic_bool_init(&SoloStop, false);
    atomic_bool_init(&SoloIncreaseDepth, false);
    SoloFlagsReady = true;
}

static void solo_release(void) {
    worker_destroy(SoloWorker);
    SoloWorker = nullptr;
    shared_histories_destroy(SoloBank);
    SoloBank = nullptr;

    PoolCounters.ctx = nullptr;
    PoolCounters.nodes = nullptr;
    PoolCounters.tb_hits = nullptr;
    PoolCounters.collect_best_move_changes = nullptr;
}

static bool solo_resize(void *ctx, size_t count) {
    (void) ctx;
    if (count > 1)
        return false;  // no thread to put a second worker on

    solo_flags_ensure();
    solo_release();

    SoloBank = shared_histories_create(1);
    if (SoloBank == nullptr)
        return false;

    const WorkerCtorInputs in = { .shared_history = SoloBank,
                                  .threads = nullptr,
                                  .thread_idx = 0,
                                  .numa_thread_idx = 0,
                                  .numa_total = 1,
                                  .numa_access_token = 0 };
    SoloWorker = worker_create(&in);
    if (SoloWorker == nullptr) {
        solo_release();
        return false;
    }
    return true;
}

// Accept every policy and bind under none: with one worker there is nothing to
// distribute, and upstream's `auto` refuses to bind a single thread anyway.
static bool solo_set_numa_policy(void *ctx, const char *policy) {
    (void) ctx;
    (void) policy;
    return true;
}

static size_t solo_count(void *ctx) {
    (void) ctx;
    return SoloWorker != nullptr ? 1 : 0;
}

static SearchWorker *solo_at(void *ctx, size_t index) {
    (void) ctx;
    return index == 0 ? SoloWorker : nullptr;
}

static void solo_clear(void *ctx) {
    (void) ctx;
    if (SoloWorker != nullptr)
        worker_clear(SoloWorker);
}

static void solo_shutdown(void *ctx) {
    (void) ctx;
    solo_release();
}

static void solo_run_main(void *ctx, WorkerJobFn job, void *job_ctx) {
    (void) ctx;
    job(job_ctx);
}

static void solo_wait_main(void *ctx) { (void) ctx; }

static void solo_start_siblings(void *ctx, WorkerJobFn job) {
    (void) ctx;
    (void) job;  // there are none
}

static void solo_wait_siblings(void *ctx) { (void) ctx; }

static void solo_set_stop(void *ctx, bool value) {
    (void) ctx;
    solo_flags_ensure();
    atomic_bool_store(&SoloStop, value);
}

static atomic_bool *solo_stop_flag(void *ctx) {
    (void) ctx;
    solo_flags_ensure();
    return &SoloStop.value;
}

static void solo_set_increase_depth(void *ctx, bool value) {
    (void) ctx;
    solo_flags_ensure();
    atomic_bool_store(&SoloIncreaseDepth, value);
}

static atomic_bool *solo_increase_depth_flag(void *ctx) {
    (void) ctx;
    solo_flags_ensure();
    return &SoloIncreaseDepth.value;
}

WorkerSetOps WorkerSet = { .ctx = nullptr,
                           .resize = solo_resize,
                           .set_numa_policy = solo_set_numa_policy,
                           .count = solo_count,
                           .at = solo_at,
                           .clear = solo_clear,
                           .shutdown = solo_shutdown,
                           .run_main = solo_run_main,
                           .wait_main = solo_wait_main,
                           .start_siblings = solo_start_siblings,
                           .wait_siblings = solo_wait_siblings,
                           .set_stop = solo_set_stop,
                           .set_increase_depth = solo_set_increase_depth,
                           .stop_flag = solo_stop_flag,
                           .increase_depth_flag = solo_increase_depth_flag };

SearchWorker *search_worker_main(void) {
    if (WorkerSet.count(WorkerSet.ctx) == 0 && !WorkerSet.resize(WorkerSet.ctx, 1))
        return nullptr;
    return WorkerSet.at(WorkerSet.ctx, 0);
}

// ---- the thread vote -----------------------------------------------------

// Return the vote M has collected: every worker whose best move is M contributes
// `score - min_score + 14`. Upstream keeps this in a hash map; a scan over the same set
// is the same arithmetic in the same order, which is what matters -- the map's iteration
// order never reaches the result.
static int64_t vote_for(Move m, int32_t min_score) {
    const size_t n = WorkerSet.count(WorkerSet.ctx);
    int64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        const RootMove *const rm = &WorkerSet.at(WorkerSet.ctx, i)->ctx.root_moves[0];
        if (rm->pv.moves[0] == m)
            total += (int64_t) rm->score - min_score + 14;
    }
    return total;
}

static bool is_decisive_exact(const RootMove *rm) {
    return rm->score != -VALUE_INFINITE && value_is_decisive((Value) rm->score)
        && !root_move_is_inexact(rm);
}

SearchWorker *search_worker_best(void) {
    const size_t n = WorkerSet.count(WorkerSet.ctx);
    if (n == 0)
        return nullptr;

    SearchWorker *best = WorkerSet.at(WorkerSet.ctx, 0);
    if (n == 1 || best->ctx.root_moves == nullptr)
        return best;

    int32_t min_score = VALUE_INFINITE;
    for (size_t i = 0; i < n; ++i) {
        const int32_t s = WorkerSet.at(WorkerSet.ctx, i)->ctx.root_moves[0].score;
        if (s < min_score)
            min_score = s;
    }

    for (size_t i = 0; i < n; ++i) {
        SearchWorker *const cand = WorkerSet.at(WorkerSet.ctx, i);
        const RootMove *const best_rm = &best->ctx.root_moves[0];
        const RootMove *const new_rm = &cand->ctx.root_moves[0];

        const int64_t best_vote = vote_for(best_rm->pv.moves[0], min_score);
        const int64_t new_vote = vote_for(new_rm->pv.moves[0], min_score);

        // An aborted depth-1 search can leave an INEXACT win or loss score, which is why
        // the decisive test also demands the score not be a bound.
        const bool best_decisive = is_decisive_exact(best_rm);
        const bool new_decisive = is_decisive_exact(new_rm);

        if (best_decisive) {
            // Pick the shortest mate / tablebase conversion.
            if (new_decisive && llabs(new_rm->score) > llabs(best_rm->score))
                best = cand;
        } else if (new_decisive
                   || (!value_is_loss((Value) new_rm->score)
                       && (new_vote > best_vote
                           || (new_vote == best_vote && new_rm->pv.length > best_rm->pv.length)))) {
            best = cand;
        }
    }
    return best;
}
