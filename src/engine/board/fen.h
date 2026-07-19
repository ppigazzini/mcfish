// Own the FEN *emit* side of the board: the FEN record and the `d`-command board
// dump.
//
// Emit is total — every reachable `Position` has a FEN — so nothing here can fail
// and nothing here validates. The one contract is on the caller: `pos_fen` writes
// an unbounded `sprintf` run and needs at least 128 bytes, which is above the
// longest record a legal position can produce (71 placement characters plus the
// four trailing fields).
//
// The emitted record must round-trip through `pos_set`: `tools/board.golden` and
// `tools/errors.golden` both pin the exact bytes of the `d` output, so a changed
// separator or a changed fullmove derivation is a golden break, not a cosmetic
// edit.
//
// Ported from zfish `engine/board/fen.zig` (formatFen), re-expressed over the live
// `Position` rather than zfish's loose primitives because mcfish has no
// caller that formats from raw board bytes. Golden:
// `Stockfish/src/position.cpp: Position::fen`.

#ifndef MCFISH_FEN_H
#define MCFISH_FEN_H

#include "position_types.h"
#include "types.h"

// Write POS's FEN record into BUF as a NUL-terminated string. BUF needs >= 128 bytes.
void pos_fen(const Position *pos, char *buf);

// Render POS as an ASCII board followed by the FEN and the key, as UCI `d` prints
// it. Truncate at BUF_LEN.
void pos_pretty(const Position *pos, char *buf, int buf_len);

#endif  // MCFISH_FEN_H
