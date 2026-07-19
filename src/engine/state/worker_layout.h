// Own the per-worker search state: the history tables, the NNUE arena, the hot search
// context, the root position and its move list.
//
// THIS IS THE BLOCK THE SEARCH'S FILE-SCOPE GLOBALS MOVED INTO. Everything here was a
// static in `search.c`, `history.c` or `evaluate.c` while the engine ran on one thread.
// Lazy-SMP runs the same iterative deepening on the same root in N workers that share
// only the transposition table and the node's shared history bank, so every field below
// must be reachable from exactly one thread -- a second worker over one copy is a data
// race on tens of megabytes, not a parallel search.
//
// A worker is one allocation and is built by `worker_construct`. It is large (the
// per-worker history tables alone are megabytes), so it is never a stack object and
// never copied.
//
// The types are the LIVE search zone's -- `SearchCtx`, `RootMoveList`, `Histories`,
// `EvalArena`. A second, parallel set of the same records is how the two shapes come to
// disagree about which one the search actually reads.
//
// Nothing here may make thread 0's behaviour depend on an address, a clock or scheduling.
// `nodes`, `tb_hits` and `best_move_changes` are read by other workers only for
// reporting; NO SEARCH DECISION MAY BE TAKEN ON ANOTHER WORKER'S COUNTER.
//
// Upstream: search.h:311 (Worker), search.h:242 (SearchManager), thread.h (ThreadPool).

#ifndef MCFISH_WORKER_LAYOUT_H
#define MCFISH_WORKER_LAYOUT_H

#include "../../platform/thread_pool.h"
#include "../../platform/thread_runtime.h"
#include "../board/position.h"
#include "../board/types.h"
#include "../eval/evaluate.h"
#include "../search/history.h"
#include "../search/root_move_build.h"
#include "../search/search_types.h"
#include "../search/timeman.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Hold the bookkeeping ONLY thread 0 has. Upstream gives the siblings a
// NullSearchManager whose one virtual does nothing; here a null `manager` says the same
// with no call. Every field is per-game or per-search state of the MANAGER, not of the
// tree, which is why a sibling needs none of it.
//
// `ponder` is polled by the search while the input thread writes it, so the two accesses
// to the one location are both atomic.
//
// Upstream: search.h:242 (SearchManager).
typedef struct {
    TimeManagement tm;
    double original_time_adjust;
    int calls_cnt;
    AtomicBool ponder;
    int32_t iter_value[4];
    double previous_time_reduction;
    Value best_previous_score;
    Value best_previous_average_score;
    bool stop_on_ponderhit;
} SearchManager;

// Seed the manager at upstream's per-game defaults: VALUE_INFINITE is "no previous
// score" and 0.85 the time-reduction seed (thread.cpp: ThreadPool::clear). Reach this on
// `ucinewgame` and at construction, NEVER per `go` -- within a game each search seeds its
// aspiration window and its falling-eval term from the previous one's result, and
// re-seeding per move searches a different tree.
void search_manager_clear(SearchManager *sm);

typedef struct SearchWorker {
    // Own this worker's history tables. `hist.shared` names its NUMA node's bank, which
    // its siblings on that node share.
    Histories hist;

    // Evaluate through this worker's own accumulator and refresh cache.
    EvalArena *eval_arena;

    // Record the net generation `eval_arena`'s refresh cache was seeded at. 0 means
    // never seeded, which is what a worker built before the first net load holds.
    uint64_t network_generation;

    // Carry the hot per-node context the whole recursion threads through.
    SearchCtx ctx;

    // Hold this worker's own copy of the root: upstream rebuilds it from the caller's FEN
    // and then overwrites the root state with the caller's, so the repetition chain
    // reached through `previous` stays the game's while the board is this worker's.
    Position root_pos;
    StateInfo root_state;
    RootMoveList rml;

    // Point at this worker's manager on thread 0 and at nothing on the siblings.
    SearchManager *manager;

    size_t thread_idx;
    size_t numa_thread_idx;
    size_t numa_total;
    size_t numa_access_token;

    ThreadPool *threads;
} SearchWorker;

static inline bool worker_is_mainthread(const SearchWorker *w) { return w->thread_idx == 0; }

#endif  // MCFISH_WORKER_LAYOUT_H
