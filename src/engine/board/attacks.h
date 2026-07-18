// Own the slider attack tables: magic bitboards and the derived square-pair geometry.
//
// Ported from zfish `engine/board/bitboard.zig` (the magic search) against the
// upstream golden `attacks.cpp`. Upstream now carries several slider backends
// selected by macro; ccfish implements the classic magic-bitboard one, which is
// what zfish proved bit-exact.
//
// A magic replaces the per-node ray walk with mask/multiply/shift/load. The
// tables are built once by attacks_init() and are READ-ONLY during search — the
// single-threaded startup init is the only writer, which is what makes them safe
// to share across the search threads M4 will add.

#ifndef CCFISH_ATTACKS_H
#define CCFISH_ATTACKS_H

#include "types.h"

// Describe one square's magic entry for one slider type.
//
// `attacks` points into a shared flat table, not into storage this struct owns:
// each square's block starts where the previous square's ended, so the whole set
// is one allocation and `attacks[index]` is a single dependent load.
typedef struct {
    Bitboard mask;      // the relevant-occupancy mask, edges excluded
    Bitboard *attacks;  // into RookTable / BishopTable
    Bitboard magic;
    unsigned shift;  // 64 - popcount(mask)
} Magic;

// Build the magic tables and the square-pair geometry. Call once, before any
// position exists. Not idempotent-safe against concurrent callers.
void attacks_init(void);

extern Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
extern Bitboard PawnAttacksBB[COLOR_NB][SQUARE_NB];
extern Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];  // exclusive of from, inclusive of to
extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];     // the full line through both, or 0

// Return the attack set of PT from S given OCCUPIED. Sliders index their magic
// table; leapers read PseudoAttacks and ignore OCCUPIED.
Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied);

// Test whether S1, S2 and S3 are collinear. Used to keep a pinned piece on its
// pin ray, which is the whole legality test for a non-king mover.
static inline bool aligned(Square s1, Square s2, Square s3) {
    return (LineBB[s1][s2] & ((Bitboard) 1 << s3)) != 0;
}

#endif  // CCFISH_ATTACKS_H
