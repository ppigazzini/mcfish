// Own the root move list the search ranks, and the per-line principal variation.
//
// A RootMove is a refutation as often as it is a line: a move that failed low still
// carries the PV that refuted it, which is why the score defaults to -VALUE_INFINITE
// rather than to zero. Two PVs are kept, not one -- `pv` is this iteration's and
// `previous_pv` is the last completed iteration's for THIS line. The follow-PV heuristic
// reads `previous_pv`, and collapsing the two into one is invisible at MultiPV 1 and
// wrong the moment MultiPV exceeds it.
//
// The list is a fixed MAX_MOVES-capacity vector, sized once at init. That is deliberate:
// the entries are addressed by pointer while a search runs, so the storage must never
// move, and the legal move count at the root is bounded by MAX_MOVES anyway.
//
// Sorting is STABLE (insertion sort), matching upstream's std::stable_sort. An unstable
// sort orders equal-scoring root moves by an implementation detail, which moves the
// bestmove without moving the node count.
//
// Upstream: search.h:47 (PVMoves), search.h:126 (RootMove), search.cpp (stable_sort of
// rootMoves). Port source: zfish src/engine/state/root_move.zig.

#ifndef MCFISH_ROOT_MOVE_H
#define MCFISH_ROOT_MOVE_H

#include "../board/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Hold one line: MAX_PLY + 1 moves at most, one per ply plus the root move.
enum { PV_MOVES_CAPACITY = MAX_PLY + 1 };

typedef struct {
    Move moves[PV_MOVES_CAPACITY];
    size_t length;
} PVMoves;

// Reset PV to empty. The move buffer is left as it stands -- `length` is the whole
// contract, exactly as upstream's clear().
void pv_moves_clear(PVMoves *pv);

// Append M. Appending past PV_MOVES_CAPACITY is a caller error.
void pv_moves_push_back(PVMoves *pv, Move m);

// Shrink to NEW_SIZE, which must not exceed the current length.
void pv_moves_resize(PVMoves *pv, size_t new_size);

// Rebuild PV as M followed by CHILD. Pass CHILD null for a one-move line.
void pv_moves_update(PVMoves *pv, Move m, const PVMoves *child);

static inline bool pv_moves_empty(const PVMoves *pv) { return pv->length == 0; }

typedef struct {
    uint64_t effort;
    Value score;
    Value previous_score;
    Value average_score;
    Value mean_squared_score;
    Value uci_score;
    bool score_lowerbound;
    bool score_upperbound;
    // Gate the aborted-MultiPV score repair: a bound score from an interrupted line must
    // not be republished as exact.
    bool previous_score_exact;
    int sel_depth;
    int tb_rank;
    Value tb_score;
    PVMoves pv;
    PVMoves previous_pv;
} RootMove;

// Seed RM at upstream's defaults with M as the only PV move.
void root_move_init(RootMove *rm, Move m);

static inline bool root_move_score_is_bound(const RootMove *rm) {
    return rm->score_lowerbound || rm->score_upperbound;
}

static inline void root_move_unset_bound_flags(RootMove *rm) {
    rm->score_lowerbound = false;
    rm->score_upperbound = false;
}

// Report an exact (non-bound) proven loss. IS_LOSS is the caller's verdict on `score`:
// the loss threshold belongs to the search's value module, so this type stays free of it.
static inline bool root_move_score_is_exact_loss(const RootMove *rm, bool is_loss) {
    return rm->score != -VALUE_INFINITE && is_loss && !root_move_score_is_bound(rm);
}

static inline bool root_move_eq(const RootMove *rm, Move m) { return rm->pv.moves[0] == m; }

// Order descending by score, then by previous score -- upstream's operator<.
bool root_move_less(const RootMove *a, const RootMove *b);

// Hold the root move list. `moves` is allocated once at MAX_MOVES entries and never
// reallocated, so a RootMove pointer stays valid for the life of the list.
typedef struct {
    RootMove *moves;
    size_t count;
    size_t capacity;
} RootMoveList;

// Allocate the fixed backing store. Return false on allocation failure, leaving LIST
// empty and safe to free.
bool root_move_list_init(RootMoveList *list);

// Release the backing store and zero LIST. Idempotent, and safe on a zeroed list.
void root_move_list_free(RootMoveList *list);

// Drop every entry, keeping the backing store.
void root_move_list_clear(RootMoveList *list);

// Append a RootMove seeded with M. Return null when the list is at capacity.
RootMove *root_move_list_push(RootMoveList *list, Move m);

// Return the entry whose first PV move is M, or null.
RootMove *root_move_list_find(RootMoveList *list, Move m);

static inline size_t root_move_list_count(const RootMoveList *list) { return list->count; }

static inline RootMove *root_move_list_at(RootMoveList *list, size_t index) {
    return &list->moves[index];
}

// Stably sort [FIRST, LAST) by root_move_less. LAST must not exceed the count.
void root_move_list_sort(RootMoveList *list, size_t first, size_t last);

#endif  // MCFISH_ROOT_MOVE_H
