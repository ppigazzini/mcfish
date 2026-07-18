// Own the read-only derived queries over a live `Position`: the scalar accessors
// that are not one-line field reads, and the snapshot fill.
//
// Every function here takes a `const Position *` and writes nothing back. That is
// the whole invariant: a query may be called at any point in the search, including
// between a `pos_do_move` and its `pos_undo_move`, and must never leave a trace.
// A "query" that caches into the Position is not one — put it in `state_setup`
// where the cache is rebuilt under a known ordering.
//
// The one-line field reads (`piece_on`, `pieces`, `checkers`, `king_square`, ...)
// stay `static inline` in `position.h`: routing them through a call here would put
// a function call on the hottest path in the tree for no separation gained.
//
// Ported from zfish `engine/board/position_query.zig`. Golden:
// `Stockfish/src/position.h` (the accessors) and `Stockfish/src/uci.cpp` (the WDL
// material model).

#ifndef CCFISH_POSITION_QUERY_H
#define CCFISH_POSITION_QUERY_H

#include "position_snapshot.h"
#include "position_types.h"
#include "types.h"

Color pos_side_to_move(const Position *pos);
bool pos_is_chess960(const Position *pos);
int pos_game_ply(const Position *pos);
bool pos_has_checkers(const Position *pos);

// Count material on the WDL model (`Stockfish/src/uci.cpp`): pawns + 3*(knights +
// bishops) + 5*rooks + 9*queens, both colors. These weights are the win-probability
// model's, NOT the search's piece values — they are deliberately different numbers
// and must not be unified with `piece_value`.
int pos_wdl_material(const Position *pos);

// Derive the snapshot from POS. Fill every field of OUT; the caller need not
// pre-zero it.
void pos_fill_snapshot(const Position *pos, PositionSnapshot *out);

// Copy the 64-square piece board only, for consumers that need the board and
// nothing else. PIECES_OUT must have room for SQUARE_NB entries.
void pos_accumulator_snapshot(const Position *pos, Piece *pieces_out);

#endif  // CCFISH_POSITION_QUERY_H
