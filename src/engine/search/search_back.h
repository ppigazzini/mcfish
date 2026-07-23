// Run the move loop and node finalization (Steps 13-21) of the main search.
//
// COMPONENT: the other half of search_main's component — see that header for why
// the cycle is the algorithm and must not be broken.
//
// search_run_back takes the pre-loop node state as a value struct (the Steps 1-12
// invariants, which it must not recompute) and owns the loop's mutable running
// state itself. It recurses into search_node for every child.
//
// Golden: `Stockfish/src/search.cpp: search<NodeType>`, the moves loop onward.

#ifndef MCFISH_SEARCH_BACK_H
#define MCFISH_SEARCH_BACK_H

#include "search_types.h"
#include "tt.h"

#include "../board/position.h"
#include "../board/types.h"

// Carry everything Steps 1-12 established. Passed by pointer, read-only except
// for what the loop shadows into its own locals (alpha, depth, best_value).
typedef struct {
    SearchCtx *ctx;
    Position *pos;
    Stack *ss;
    Stack *ss1;
    Histories *hist;
    Color us;

    Value alpha;
    Value beta;
    int depth;
    Value best_value;
    Value max_value;

    Move excluded_move;
    Move tt_move;
    Value tt_value;
    int tt_depth;
    Bound tt_bound;
    bool tt_capture;

    // Keep the bool fields split into runs of at most two: a run of four or
    // more adjacent byte stores tempts clang's SLP vectorizer into assembling
    // them through k-mask registers (~15 mask-domain ops per node where four
    // plain byte stores do), and this struct is built once per interior node.
    int correction_value;
    bool cut_node;
    bool pv_node;

    Value unadjusted_static_eval;
    bool root_node;
    bool all_node;

    TTEntry *writer;
    bool improving;
    bool prior_capture;

    Key pos_key;
    int prev_sq;  // SQ_NONE when the previous ply made no move
} SearchNodeState;

Value search_run_back(const SearchNodeState *nd);

#endif  // MCFISH_SEARCH_BACK_H
