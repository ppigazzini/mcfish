// Own the stop conditions and the root-move bookkeeping.
//
// Everything here operates on the SearchCtx plus the injected clock and the
// shared stop / ponder flags. None of it calls into the recursion, so it stays a
// clean cluster the node bodies can import one-way.
//
// The invariant that makes a fixed-depth run reproducible: the clock is read only
// inside check_time, which fires once every `calls_cnt` nodes. Move a clock read
// into the recursion and the node count becomes a function of machine speed.
//
// Ported from zfish `engine/search/search_control.zig`.
// Golden: `Stockfish/src/search.cpp: check_time` / `search<Root>` bookkeeping.

#ifndef CCFISH_SEARCH_CONTROL_H
#define CCFISH_SEARCH_CONTROL_H

#include "search_types.h"

#include "../board/types.h"

#include <stdint.h>

// Decrement the call counter and raise the stop flag when the budget is spent.
// A no-op off the main thread, which has no counter.
void check_time(SearchCtx *ctx);

// Do the per-move root bookkeeping: find MOVE's RootMove in [pv_idx, pv_last),
// update its effort / averageScore / meanSquaredScore, and on a PV move store the
// score, the bound flags and the PV.
void root_update(SearchCtx *ctx,
                 Move move,
                 Value value,
                 uint64_t nodes_delta,
                 int move_count,
                 Value alpha,
                 Value beta,
                 const PVMoves *child_pv);

// Read the root TT move: rootMoves[pvIdx].pv[0].
static inline Move root_tt_move(const SearchCtx *ctx) {
    return ctx->root_moves[ctx->pv_idx].pv.moves[0];
}

// Report whether MOVE is one of the root moves still being searched.
bool root_in_list(const SearchCtx *ctx, Move move);

// Load the shared stop flag.
bool search_stopped(const SearchCtx *ctx);

// Compare MOVE against the previous iteration's PV for the follow-PV test.
static inline bool in_last_iter_pv(const SearchCtx *ctx, int ply_minus_1, Move move) {
    const PVMoves *const pv = &ctx->last_iter_pv;
    const size_t idx = (size_t) ply_minus_1;
    return idx < pv->length && pv->moves[idx] == move;
}

#endif  // CCFISH_SEARCH_CONTROL_H
