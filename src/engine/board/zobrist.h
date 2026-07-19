// Own the Zobrist key tables and the fixed-seed PRNG that fills them.
//
// THE SEED AND THE DRAW ORDER ARE LOAD-BEARING. Every position key, every
// transposition-table probe, the bench signature and every golden in `tools/` are
// functions of these tables. `zobrist_init` seeds xorshift64* with 1070372 and
// draws, in this exact order:
//
//   1. Zobrist_psq[pc][sq] for the 12 REAL pieces, in upstream's Pieces[] order,
//      sq = SQ_A1..SQ_H8 — 12 * 64 draws. The encoding gaps at 7 and 8 are
//      SKIPPED, exactly as upstream's `for (Piece pc : Pieces)` skips them
//   2. Zobrist_psq[W_PAWN][rank 8] and [B_PAWN][rank 1] are then ZEROED — a pawn
//      reaches those squares only by promoting and never rests there, so upstream
//      zeroes them to make the promotion XOR cancel implicitly
//   3. Zobrist_enpassant[0..7]
//   4. Zobrist_castling[0..15]
//   5. Zobrist_side
//   6. Zobrist_no_pawns
//
// Drawing for the gaps, reordering the draws, skipping the zero-fill, or reseeding
// a second copy of the PRNG elsewhere shifts every key downstream and silently
// invalidates the whole anchor set: the gaps alone would consume 128 extra values
// and move every key from B_PAWN on. The tables are written once at startup and
// READ-ONLY thereafter.
//
// Golden: `Stockfish/src/position.cpp: Position::init` (:123-131).

#ifndef MCFISH_ZOBRIST_H
#define MCFISH_ZOBRIST_H

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

#endif  // MCFISH_ZOBRIST_H
