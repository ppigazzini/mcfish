// Inject the worker set the search runs on.
//
// Lazy-SMP needs OS threads, and on a multi-socket host it needs them bound to NUMA
// nodes and their history banks first-touched there. Both are host services, so the
// engine names what it needs and the host supplies it -- the shape output_sink.h,
// time_source.h and arena_source.h already use, one level up: this seam hands over a
// whole set rather than a single function.
//
// WHAT STAYS ON THIS SIDE. The thread VOTE is chess policy, not dispatch: it reads
// RootMoves and picks the worker whose line the engine will play, so it lives here
// (search_worker_best) and reaches the set through `count` and `at`. Only the
// lifecycle and the dispatch cross over.
//
// THE DEFAULT IS A REAL ONE-WORKER SET, NOT A STUB. Unregistered, the engine builds a
// single worker and runs every job inline on the calling thread. That is not a
// degraded mode for the search itself: `search_go` blocks and drives worker 0 inline
// even with a pool attached, so a one-worker run takes the same path and searches the
// same tree. It is what lets src/engine/ search on its own -- the test binary links no
// platform object and takes exactly this path -- and it is why `Threads 1` is
// bit-identical to no pool at all.
//
// `resize` above one worker is the one thing the default cannot do, and it says so by
// returning false rather than silently searching with fewer threads than asked for.
//
// Upstream: thread.cpp (ThreadPool::set, ::clear, ::start_searching, ::get_best_thread).

#ifndef MCFISH_WORKER_SET_H
#define MCFISH_WORKER_SET_H

#include "../state/worker_layout.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

// Run one unit of work on a worker's thread. A null context is legal.
typedef void (*WorkerJobFn)(void *job_ctx);

typedef struct {
    void *ctx;

    // Rebuild the set at COUNT workers, replacing any current one. Return false on an
    // allocation failure or a refused spawn, which must leave the set EMPTY rather
    // than partly built -- search_go rebuilds a one-worker set instead of searching
    // nothing. A rebuild drops every history bank, which is why upstream reaches it
    // only from the `Threads` option.
    bool (*resize)(void *ctx, size_t count);

    // Install the NumaPolicy the next resize binds under. Return false when the string
    // names no node at all, leaving the previous topology in place.
    bool (*set_numa_policy)(void *ctx, const char *policy);

    size_t (*count)(void *ctx);
    SearchWorker *(*at)(void *ctx, size_t index);

    // Per-game clear across the set, and full teardown at process exit.
    void (*clear)(void *ctx);
    void (*shutdown)(void *ctx);

    // Worker 0 carries the main search; the rest are siblings. `run_main` may run the
    // job inline -- the caller always pairs it with `wait_main` before reading a
    // result, so a synchronous implementation is correct.
    void (*run_main)(void *ctx, WorkerJobFn job, void *job_ctx);
    void (*wait_main)(void *ctx);
    void (*start_siblings)(void *ctx, WorkerJobFn job);
    void (*wait_siblings)(void *ctx);

    // The two flags every worker's depth loop reads, handed over BY ADDRESS. A getter
    // would put a function call on the per-node abort check; SearchCtx holds the raw
    // `atomic_bool *` and polls it directly, so the seam must yield the same storage
    // every worker writes through. Sequentially consistent -- `stop` is raised by one
    // thread and must be seen by every other (see state/atomic.h).
    void (*set_stop)(void *ctx, bool value);
    void (*set_increase_depth)(void *ctx, bool value);
    atomic_bool *(*stop_flag)(void *ctx);
    atomic_bool *(*increase_depth_flag)(void *ctx);
} WorkerSetOps;

extern WorkerSetOps WorkerSet;

// Return worker 0, building a one-worker set first if none exists. Null when even that
// could not be built.
SearchWorker *search_worker_main(void);

// Pick the worker whose move the engine will play -- upstream's ThreadPool::get_best_thread
// (thread.cpp). Chess policy over the set, not dispatch, which is why it is on this side
// of the seam.
SearchWorker *search_worker_best(void);

#endif  // MCFISH_WORKER_SET_H
