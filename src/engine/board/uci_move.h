// Translate between the internal Move encoding and UCI long algebraic notation.
//
// The internal encoding stores castling as king-captures-rook; UCI wants the
// king's real destination in standard chess and the rook square in Chess960. That
// asymmetry lives here and nowhere else.

#ifndef CCFISH_UCI_MOVE_H
#define CCFISH_UCI_MOVE_H

#include "position.h"
#include "types.h"

// Write M as UCI into BUF (needs >= 6 bytes). Emit "(none)" for MOVE_NONE.
void move_to_uci(const Position *pos, Move m, char *buf);

// Parse STR against POS's legal moves. Return MOVE_NONE when no legal move matches,
// which is also how an illegal or malformed token is reported.
Move move_from_uci(const Position *pos, const char *str);

#endif  // CCFISH_UCI_MOVE_H
