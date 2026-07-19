// Own quiescence search, and the low-level node primitives the main search
// shares with it.
//
// qsearch_node is a call-graph leaf: it only ever recurses into itself, never
// into the main alpha-beta node. That is what lets the whole search zone's import
// cycle stay confined to search_main <-> search_back. The shared primitives live
// here for the same reason — both node bodies need them, and this is the half of
// the pair with no dependency on the other.
//
// Ported from zfish `engine/search/search_qsearch.zig`.
// Golden: `Stockfish/src/search.cpp: qsearch`.

#ifndef MCFISH_SEARCH_QSEARCH_H
#define MCFISH_SEARCH_QSEARCH_H

#include "search_types.h"

#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>

// Reach a neighbouring stack frame. Spelled out rather than written `ss + n` at
// every call site so the direction is never ambiguous in a dense expression.
static inline Stack *ss_add(Stack *ss, size_t n) { return ss + n; }
static inline Stack *ss_sub(Stack *ss, size_t n) { return ss - n; }

static inline void pv_clear(PVMoves *pv) { pv->length = 0; }

// Prepend MOVE to CHILD and store the result in PV. CHILD may be null, which is
// the case where the move ran no PV search: the PV is then just the move.
void pv_update(PVMoves *pv, Move move, const PVMoves *child);

// Report whether MOVE merely shuffles a piece back and forth — the guard that
// keeps a singular extension out of a repetition dance.
bool is_shuffling(const Position *pos, const Stack *ss, Move move);

// Test capture the way the correction-history gate does: upstream's
// Position::capture, which does NOT fold in queen promotions.
static inline bool pos_capture(const Position *pos, Move m) {
    const MoveType t = move_type(m);
    return (piece_on(pos, move_to(m)) != NO_PIECE && t != CASTLING) || t == EN_PASSANT;
}

// Blend the six correction-history reads for the current node.
int search_correction_value(Histories *h, const Position *pos, const Stack *ss);

// Hash the halfmove clock into the key past move 14, so a position reached with a
// different rule50 count cannot reuse a TT entry whose score the rule invalidates.
Key adjust_key50(const Position *pos);

void tt_move_history_update(Histories *h, int bonus);

// Read one continuation-history page entry.
static inline int cont_val(const int16_t *page, Piece pc, Square to) {
    return page[(size_t) pc * SQUARE_NB + (size_t) to];
}

// Search the captures (and, in check, the evasions) until the position is quiet.
Value qsearch_node(SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, bool pv_node);

#endif  // MCFISH_SEARCH_QSEARCH_H
