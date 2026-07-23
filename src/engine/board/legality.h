// Own the three move-validity questions the board answers: is a 16-bit word a
// move at all, does it leave our king safe, and does the capture sequence it
// starts win material.
//
// THE THREE ARE LAYERED AND THE ORDER IS AN INVARIANT.
//
//   pos_pseudo_legal  defined on ANY 16-bit word. A transposition-table hit hands
//                     back a move word that may belong to a different position
//                     entirely, so this is the only one safe to call on unchecked
//                     input.
//   pos_legal         defined ONLY on pseudo-legal input. It assumes a piece of
//                     ours stands on `from` and that the move is geometrically
//                     available; on anything else it reads the wrong square and
//                     answers confidently.
//   see_ge            defined only on pseudo-legal input, and only meaningful for
//                     NORMAL moves — every other move type short-circuits to
//                     `0 >= threshold`.
//
// Calling `pos_legal` on a raw TT move is the classic way to lose a search to a
// hash collision. Filter with `pos_pseudo_legal` first, always.
//
// `see_ge` reads `st->pinners` and `st->blockers`, so it is only correct while the
// cached check state matches the board — that is, after `set_check_info`, never
// mid-`pos_do_move`.
//
// Golden: `Stockfish/src/position.cpp: Position::legal`,
// `Position::pseudo_legal`, `Position::see_ge`.

#ifndef MCFISH_LEGALITY_H
#define MCFISH_LEGALITY_H

#include "position_types.h"
#include "types.h"

// Test whether M is a move POS could make at all, ignoring king safety. Total over
// every 16-bit word.
bool pos_pseudo_legal(const Position *pos, Move m);

// Test whether M — which MUST be pseudo-legal for POS — leaves our king safe.
bool pos_legal(const Position *pos, Move m);

// Test whether the capture sequence starting with M gains at least THRESHOLD.
// Static: it resolves the exchange on `to` and nothing else.
bool see_ge(const Position *pos, Move m, int threshold);

// Test whether M — which MUST be pseudo-legal for POS — gives check. Reads the
// check squares and blockers cached by set_check_info, so it carries the same
// freshness requirement as `see_ge`. `pos_do_move` TRUSTS its gives_check
// argument to equal this predicate; passing anything else corrupts the child's
// checkers set. Golden: upstream `Position::gives_check` (position.cpp).
bool pos_gives_check(const Position *pos, Move m);

#endif  // MCFISH_LEGALITY_H
