// Own the Zobrist key tables and the fixed-seed PRNG that fills them.
//
// THE SEED AND THE DRAW ORDER ARE LOAD-BEARING. Every position key, every
// transposition-table probe, the bench signature and every golden in `tools/` are
// functions of these tables. `zobrist_init` seeds xorshift64* with 1070372 and
// draws, in this exact order:
//
//   1. Zobrist_psq[pc][sq] for pc = W_PAWN..B_KING (1..14, INCLUDING the two
//      unused encoding gaps 7 and 8) and sq = SQ_A1..SQ_H8 — 14 * 64 draws
//   2. Zobrist_enpassant[0..7]
//   3. Zobrist_castling[0..15]
//   4. Zobrist_side
//   5. Zobrist_no_pawns
//
// Skipping the gaps, reordering the draws, or reseeding a second copy of the PRNG
// elsewhere shifts every key downstream and silently invalidates the whole anchor
// set. The tables are written once at startup and READ-ONLY thereafter.
//
// Ported from zfish `engine/board/zobrist.zig`, whose draw order differs (it skips
// pieces 7 and 8 and zeroes the pawn promotion ranks); ccfish's existing
// `position_init` is the sequence in force here and the one reproduced.
// Golden: `Stockfish/src/position.cpp: Position::init`.

#ifndef CCFISH_ZOBRIST_H
#define CCFISH_ZOBRIST_H

#include "types.h"

// Fill the tables. Call once at startup, before any position is set up. Idempotent:
// each call rebuilds every table from the same seed.
void zobrist_init(void);

extern Key Zobrist_psq[PIECE_NB][SQUARE_NB];
extern Key Zobrist_enpassant[FILE_NB];
extern Key Zobrist_castling[16];
extern Key Zobrist_side;

// Seed the pawn key, so a position with no pawns still has a distinct pawn key
// rather than the zero every empty XOR fold produces.
extern Key Zobrist_no_pawns;

#endif  // CCFISH_ZOBRIST_H
