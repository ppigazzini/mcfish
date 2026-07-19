// Own the FEN *decode* side of the board: build a live `Position` from a record.
//
// PARSING IS TOTAL. `pos_set` is defined on every byte string a UCI client can
// send, and it never reads past the terminator, never places a piece off the
// board, and never leaves the caller a half-built position it is expected to
// notice. It answers `true` or `false`; `false` means the caller keeps whatever
// position it had, which is the fallback `tools/errors.golden` pins (`position fen
// not-a-fen` must leave the previous position intact and print it unchanged).
//
// The acceptance set is a contract, not a quality bar. `pos_set` rejects exactly:
// a rank that overflows or underflows, a placement that ends off the last rank, a
// piece letter outside " PNBRQK  pnbrqk", anything but one king per side, a
// side-to-move field that is not `w` or `b`, a castling field naming a square with
// no rook of that color, and a malformed en-passant field. Everything else is
// accepted. Widening or narrowing that set changes which UCI inputs reach the
// search and breaks the golden.
//
// The en-passant square survives only when the capture is actually available. A
// FEN that states one unconditionally would otherwise perturb the key and
// desynchronise every anchor built on it — the same rule `pos_do_move` applies
// when it sets the square.
//
// The acceptance set is the one mcfish's `position.c: pos_set` already
// established. Stricter rejections (pawns on the back ranks, >32 pieces,
// unreachable promotion counts, counter range checks, king-can-be-captured) are
// NOT applied here, because they would change which of `tools/cases/errors.uci`
// and `tools/cases/board.uci` are accepted. Golden:
// `Stockfish/src/position.cpp: Position::set`.

#ifndef MCFISH_FEN_PARSE_H
#define MCFISH_FEN_PARSE_H

#include "position_types.h"
#include "types.h"

// Set POS from the FEN record FEN, anchoring its state chain at SI. Zero both
// records first, so a rejected record leaves no field of the previous position
// behind.
//
// Return false and leave POS unspecified when the record is malformed. The caller
// must discard POS on false, never inspect it.
bool pos_set(Position *pos, const char *fen, bool chess960, StateInfo *si);

#endif  // MCFISH_FEN_PARSE_H
