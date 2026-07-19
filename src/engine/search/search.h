// Drive the search: iterative deepening over alpha-beta with quiescence.
//
// The search is single-threaded and deterministic: for a fixed depth and a fixed
// TT state it visits the same nodes in the same order every run. That property is
// what the `signature` gate asserts, so anything that makes node counts depend on
// wall-clock time (a time check inside the recursion, a nondeterministic move
// order) breaks the gate rather than merely slowing things down.

#ifndef CCFISH_SEARCH_H
#define CCFISH_SEARCH_H

#include "../board/position.h"
#include "../board/types.h"

typedef struct {
    int depth;        // fixed-depth limit, 0 for none
    int movetime_ms;  // fixed time per move, 0 for none
    int time_ms[COLOR_NB];
    int inc_ms[COLOR_NB];
    int moves_to_go;
    uint64_t nodes;  // node limit, 0 for none
    bool infinite;
    bool ponder;
} SearchLimits;

typedef struct {
    uint64_t nodes;
    int depth_reached;
    Value score;
    Move best_move;
    Move ponder_move;
    int elapsed_ms;
} SearchResult;

// Search POS under LIMITS and return the outcome. Emit `info` lines through the
// sink installed by search_set_output; emit nothing when none is installed.
SearchResult search_go(Position *pos, const SearchLimits *limits);

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

// Request that the running search stop at the next check. Safe to call from the
// input thread while search_go runs.
void search_stop(void);

// Reset everything whose lifetime is a GAME rather than a search: the history
// block and the per-game manager scalars. Upstream does this in
// ThreadPool::clear, reached from `ucinewgame` and nowhere else. Call it there,
// and at startup -- never per `go`, or every search starts from a blank history
// and the engine searches a different tree than upstream.
void search_clear(void);

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

#endif  // CCFISH_SEARCH_H
