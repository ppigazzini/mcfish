// Own the read-only board properties: the scalar facts a caller reads off a live
// Position without touching its internals, and the piece-array copy the NNUE
// accumulator takes.
//
// Every function here is total over a `const Position *` and mutates nothing, so
// this stays a leaf over position.h — it must never gain a make/unmake caller or a
// pointer into Position's interior. That read-only-ness is what lets the eval and
// the search share one definition of "what is on the board" instead of each
// reaching into the struct.
//
// Ported from zfish `engine/board/position_query.zig` (the scalar accessors and
// accumulatorSnapshot). Golden: `Stockfish/src/position.h` (side_to_move,
// is_chess960, game_ply, checkers, piece_array) and `Stockfish/src/uci.cpp`
// (the WDL material count).

#ifndef MCFISH_BOARD_PROPS_H
#define MCFISH_BOARD_PROPS_H

#include "position.h"
#include "types.h"

Color board_side_to_move(const Position *pos);
bool board_is_chess960(const Position *pos);
int board_game_ply(const Position *pos);
bool board_has_checkers(const Position *pos);

// Count the WDL-model material: pawns + 3*(knights + bishops) + 5*rooks +
// 9*queens, both colors (zfish position_query.zig:464, upstream uci.cpp).
int board_wdl_material(const Position *pos);

// Copy the 64-square piece board into PIECES_OUT, for the NNUE piece-count and
// accumulator callers that need the board and nothing else.
void board_copy_pieces(const Position *pos, Piece *pieces_out);

#endif  // MCFISH_BOARD_PROPS_H
