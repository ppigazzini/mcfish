// Drive the search: iterative deepening over alpha-beta with quiescence.
//
// The search is single-threaded and deterministic: for a fixed depth and a fixed
// TT state it visits the same nodes in the same order every run. That property is
// what the `signature` gate asserts, so anything that makes node counts depend on
// wall-clock time (a time check inside the recursion, a nondeterministic move
// order) breaks the gate rather than merely slowing things down.

#ifndef MCFISH_SEARCH_H
#define MCFISH_SEARCH_H

#include "../board/position.h"
#include "../board/types.h"

typedef struct {
    int depth;        // fixed-depth limit, 0 for none
    int movetime_ms;  // fixed time per move, 0 for none
    int time_ms[COLOR_NB];
    int inc_ms[COLOR_NB];
    int moves_to_go;
    uint64_t nodes;  // node limit, 0 for none
    // `go mate N`: stop once the best score is a mate in at most N MOVES -- the
    // depth loop compares 2*N against the ply distance, which is why the doubling
    // is upstream's and not a unit slip. 0 for none. The search zone has carried
    // this the whole time (search_setup wires it to SearchIdState::limits_mate and
    // search_id acts on it); only the shell never filled it in.
    int mate;
    bool infinite;
    bool ponder;
    // Root moves the caller restricted the search to, IN THE ORDER GIVEN. Upstream
    // builds its root list straight from `limits.searchmoves` and falls back to the
    // generator only when that list is empty (thread.cpp:301-314), so the order is
    // observable: it decides which move is searched first at depth one. Filtering the
    // generator instead would silently re-impose generation order.
    //
    // Already converted to Moves by the shell, which is where the position and the
    // UCI string parser both are; upstream carries the strings and converts in
    // start_thinking, against the same position. An unconvertible or illegal move
    // converts to MOVE_NONE and is dropped, which is upstream's `to_move` returning
    // none -- and is why upstream needs no separate legality filter here.
    Move searchmoves[MAX_MOVES];
    size_t searchmoves_count;
    // Wall clock stamped when the `go` line was parsed, as early as possible
    // (upstream uci.cpp:190). 0 means unset, and search_go stamps its own entry
    // instead -- the path bench and the tests take, where elapsed time is not scored.
    int64_t start_time;
} SearchLimits;

typedef struct {
    uint64_t nodes;
    int depth_reached;
    Value score;
    Move best_move;
    Move ponder_move;
    int elapsed_ms;
} SearchResult;

// Search POS under LIMITS and return the outcome, blocking until it completes. This
// is search_go_start followed by search_wait: the synchronous entry the bench and
// the tests take, where the caller wants the result in hand on return. Emit `info`
// lines through the sink installed by search_set_output; emit nothing when none is
// installed. POS must outlive the call.
SearchResult search_go(Position *pos, const SearchLimits *limits);

// Set the search up on the calling thread and hand it to worker 0's OS thread, then
// return WITHOUT waiting. This is what keeps the UCI loop reading stdin during a
// search, so a `stop`/`quit`/`ponderhit` is seen while the search runs. A second call
// while a search is in flight is ignored (upstream ignores a `go` during search).
// POS must outlive the search: the setup copies the root into each worker, so the
// running search never reads POS again, but the caller must not free it early.
void search_go_start(Position *pos, const SearchLimits *limits);

// Block until the search started by search_go_start has finished and published its
// result and node count. A no-op when nothing is running. Call before reading the
// result, and before any teardown that frees what a running search reads (the TT,
// the net).
void search_wait(void);

// Report whether a search is in flight (dispatched to worker 0 and not yet finished).
bool search_is_running(void);

// Report whether an in-flight search is unbounded (infinite or pondering) and so will
// not stop on its own. `quit` reads this to decide whether it must raise stop -- a
// bounded search it instead waits out, keeping that search's output deterministic.
bool search_running_unbounded(void);

// Handle `ponderhit`: clear the ponder flag so a `go ponder` search begins enforcing
// its time limits. Safe to call from the input thread; a no-op when no search runs.
void search_ponderhit(void);

// Install the line sink. Injected rather than hardcoded to stdout so the gates can
// capture output without a subprocess, and so the search zone never calls printf.
void search_set_output(void (*emit)(const char *line));

// Install the shell's UCI option table as the source the search reads for
// MultiPV, Skill Level, UCI_Elo, Move Overhead, nodestime, Ponder and
// UCI_ShowWDL.
//
// Pass nullptr to fall back to upstream's registered defaults. The fallback is
// not neutral and must not be mistaken for one: the search reads these values on
// every `go`, so a source that answers 0 to "MultiPV" searches no PV line at all
// and 0 to "Skill Level" applies the maximum handicap. Installing a real model
// must therefore replace the default set wholesale, never merge with it.
void search_set_option_source(int (*option_int_by_name)(const char *name));

// Install the monotonic clock the zone reads, in milliseconds. Reading an OS clock
// is a platform service, so the HOST supplies it -- the same shape
// search_set_output uses, and registered from the same place (engine_init).
//
// Unregistered, the zone falls back to the per-call counter in time_source.h. That
// keeps a headless caller deterministic and every ordering property intact, but the
// UNIT is ticks rather than milliseconds, so any time-LIMITED search needs this
// installed. Every root that is time-limited comes through the shell, which
// installs it before the first command.
void search_set_time_source(int64_t (*now_ms_fn)(void));

// Install the page allocator the engine's big arenas come from -- the transposition
// table, the shared history bank and each SearchWorker block. Both pointers or
// neither; a block must be released by the implementation that produced it.
//
// ALLOC MUST RETURN ZEROED MEMORY ALIGNED TO AT LEAST 64. tt_clear and history_clear
// both read a fresh arena as zero, and the accumulator indexes from a cache-line
// boundary. Unregistered, the zone falls back to an aligned, zeroed malloc, which is
// correct and simply gives up the host's huge-page hint.
void search_set_arena_source(void *(*alloc_fn)(size_t size), void (*free_fn)(void *ptr));

// Request that the running search stop at the next check. Safe to call from the
// input thread while search_go runs.
void search_stop(void);

// Reset everything whose lifetime is a GAME rather than a search: the history
// block and the per-game manager scalars. Upstream does this in
// ThreadPool::clear, reached from `ucinewgame` and nowhere else. Call it there,
// and at startup -- never per `go`, or every search starts from a blank history
// and the engine searches a different tree than upstream.
void search_clear(void);

// Rebuild the worker set at COUNT threads, replacing any current one. Return false on an
// allocation failure or a refused spawn, which leaves the set EMPTY -- the next
// `search_go` rebuilds a one-thread set rather than searching nothing.
//
// A rebuild drops every history table, so reach it the way upstream does: from the
// `Threads` option, whose change is a new pool.
[[nodiscard]] bool search_set_threads(size_t count);

// Install the NumaPolicy the next `search_set_threads` binds under. Accept `auto`,
// `none`, `system`, `hardware` or an explicit topology string. Return false when the
// string names no node at all, leaving the previous topology in place.
[[nodiscard]] bool search_set_numa_policy(const char *policy);

// Release everything the search owns: the worker block, its NNUE arena and the shared
// history bank. Call once at process exit, after the last search has returned -- the
// next `search_go` rebuilds them, so this is teardown and not a reset.
void search_shutdown(void);

// Report the node count the LAST completed search published, and reset it.
//
// bench sums this rather than a return value because that is the number
// upstream's `on_update_full` capture accumulates (uci.cpp:270). Reset before
// each search: a root with no legal moves publishes nothing, and an unreset
// counter would then count its predecessor twice.
uint64_t search_last_nodes_searched(void);
void search_reset_last_nodes_searched(void);

// Count the leaves of the legal move tree at DEPTH, printing the per-move split.
uint64_t perft(Position *pos, int depth, bool root);

#endif  // MCFISH_SEARCH_H
