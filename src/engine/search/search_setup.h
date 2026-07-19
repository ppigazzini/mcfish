// Set up one search: the stack sentinels, the SearchCtx, the time budget and the
// skill level.
//
// Everything here runs exactly once per `go`, before the first node. That is the
// boundary it defends: the recursion must never re-derive any of it, because a
// value re-read mid-tree (a clock, an option, the reductions table) makes the
// node count a function of when it was read rather than of the tree.
//
// Golden: `Stockfish/src/search.cpp: Search::Worker::start_searching`.

#ifndef MCFISH_SEARCH_SETUP_H
#define MCFISH_SEARCH_SETUP_H

#include "root_move_build.h"
#include "search_types.h"
#include "timeman.h"

#include "../board/position.h"

#include <stdatomic.h>
#include <stddef.h>

// Zero STACK and establish the 7 sentinel frames below the root: an all-NO_PIECE
// continuation page and a VALUE_NONE static eval on each, then ply numbering from
// STACK[STACK_PAD] upward. ROOT_PV is installed on the root frame.
void search_stack_init(Stack *stack, size_t count, Histories *h, PVMoves *root_pv);

// Fill CTX for a fresh search: the reductions table, the zeroed counters, the
// root move list and the stop flag. LIMITS is copied, not aliased.
void search_ctx_init(SearchCtx *ctx,
                     Histories *h,
                     Position *root_pos,
                     const SearchZoneLimits *limits,
                     const RootMoveList *rml,
                     atomic_bool *stop);

// Resolve this search's time budget into TM and write the effective clock back
// into CTX->limits, then advance the TT generation. ORIGINAL_TIME_ADJUST is
// per-game state and is updated in place on the move that fixes it.
void search_tm_init(SearchCtx *ctx, TimeManagement *tm, double *original_time_adjust);

// Wire CTX->time_state to the flags check_time reads. Call after search_tm_init.
void search_time_state_init(SearchCtx *ctx,
                            const TimeManagement *tm,
                            int *calls_cnt,
                            atomic_bool *ponder,
                            bool *stop_on_ponderhit);

// Compute the skill level: interpolated from UCI_Elo when UCI_LimitStrength is
// set, otherwise the raw Skill Level option.
double search_skill_level(void);

// Snapshot the iterative-deepening scalars for one search.
void search_id_state_init(SearchIdState *id,
                          const SearchCtx *ctx,
                          TimeManagement *tm,
                          atomic_bool *ponder,
                          bool *stop_on_ponderhit,
                          atomic_bool *increase_depth);

#endif  // MCFISH_SEARCH_SETUP_H
