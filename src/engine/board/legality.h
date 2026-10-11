// Own the three move-validity questions the board answers: is a 16-bit word a
// legal move here, does it give check, and does the capture sequence it starts win
// material.
//
// THE QUESTIONS ARE LAYERED AND THE ORDER IS AN INVARIANT.
//
//   pos_legal         defined on ANY 16-bit word. A transposition-table hit hands
//                     back a move word that may belong to a different position
//                     entirely, so this is the only one safe to call on unchecked
//                     input. Every generator emits only legal moves, so nothing a
//                     generator returns needs it.
//   pos_gives_check   defined only on legal input.
//   see_ge            defined only on legal input, and only meaningful for NORMAL
//                     moves — every other move type short-circuits to
//                     `0 >= threshold`.
//
// `pos_legal`, `pos_gives_check` and `see_ge` read the pins, check squares and
// threats cached by `set_extra_bitboards`, so they are only correct while that
// state matches the board — never mid-`pos_do_move`.
//
// Golden: `Stockfish/src/position.cpp: Position::legal`, `Position::gives_check`,
// `Position::see_ge`.

#ifndef MCFISH_LEGALITY_H
#define MCFISH_LEGALITY_H

#include "position_types.h"
#include "types.h"

// Test whether M is a legal move in POS. Total over every 16-bit word.
bool pos_legal(const Position *pos, Move m);

// Test whether the capture sequence starting with M gains at least THRESHOLD.
// Static: it resolves the exchange on `to` and nothing else.
bool see_ge(const Position *pos, Move m, int threshold);

// Test whether M — which MUST be legal for POS — gives check. Reads the check
// squares and blockers cached by set_extra_bitboards, so it carries the same
// freshness requirement as `see_ge`. `pos_do_move` TRUSTS its gives_check
// argument to equal this predicate; passing anything else corrupts the child's
// checkers set. Golden: upstream `Position::gives_check` (position.cpp).
bool pos_gives_check(const Position *pos, Move m);

#endif  // MCFISH_LEGALITY_H
