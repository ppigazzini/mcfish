// Drive iterative deepening: the depth loop, the aspiration window, MultiPV, the
// skill handicap and the per-iteration time decision.
//
// The aspiration loop is the part that is easy to get subtly wrong: on a fail low
// the window drops its lower edge and RESETS the fail-high counter; on a fail
// high it raises the upper edge and keeps counting, and that counter is what
// shortens the re-searched depth. Both edges grow by 47/128 per re-search. Change
// the order of those updates and the node count moves without the move changing.
//
// Ported from zfish `engine/search/search_id_loop.zig` + the loop helpers in
// `search_id.zig`. Golden: `Stockfish/src/search.cpp: iterative_deepening`.

#ifndef MCFISH_SEARCH_ID_H
#define MCFISH_SEARCH_ID_H

#include "search_types.h"

#include "../board/types.h"

#include <stddef.h>
#include <stdint.h>

// Emit the "currmove" line only past this many nodes, as upstream does.
enum : uint64_t { ID_NODES_LIMIT_OUTPUT = 10000000 };

// Order RootMoves descending by (score, previous_score).
bool root_less(const RootMove *a, const RootMove *b);

// Insertion-sort root_moves[lo, hi) stably: equal elements keep their order.
void stable_sort_root(RootMove *rm, size_t lo, size_t hi);

// Rotate the first RootMove whose pv[0] == TARGET to the front.
void move_to_front(RootMove *rm, size_t count, Move target);

// Run the depth loop. Return true when a complete MultiPV info line was emitted
// for the final iteration, so the caller knows not to repeat it.
bool iterative_deepening(SearchCtx *ctx, SearchIdState *id);

#endif  // MCFISH_SEARCH_ID_H
