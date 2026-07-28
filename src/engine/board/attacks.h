// Own the slider attack tables: magic bitboards and the derived square-pair geometry.
//
// The magic search runs against the upstream golden `attacks.cpp`. Upstream now
// carries several slider backends selected by macro; mcfish implements the
// classic magic-bitboard one.
//
// The declarations here are the C form of upstream's header-side surface, which
// this file owns in full: attacks.h:42 (Attacks::init), attacks.h:197
// (sliding_attack), attacks.h:293 (attacks_bb over an occupancy), attacks.h:174
// and attacks.h:179 (line_bb, between_bb) and attacks.h:191 (safe_destination,
// mcfish's safe_step). Cited with :line deliberately — mcfish has an attacks.h of
// its own, so a bare basename is ambiguous and tools/upstream_map.py skips it
// rather than guess, which is how this file read as unported surface.
//
// A magic replaces the per-node ray walk with mask/multiply/shift/load. The
// tables are built once by attacks_init() and are READ-ONLY during search — the
// single-threaded startup init is the only writer, which is what makes them safe
// to share across the search threads M4 will add.

#ifndef MCFISH_ATTACKS_H
#define MCFISH_ATTACKS_H

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

// Return BOTH slider attack sets for S in one call: `.bishop` and `.rook`.
//
// At AVX2 and above this is upstream's DUAL HYPERBOLA QUINTESSENCE (attacks.h:91,
// `#elif defined(USE_AVX2)` at attacks.h:35): the file, diagonal and antidiagonal
// masks sit in one 32-byte struct, one 256-bit load and a single subtract/reverse/xor
// pass produces all three ray sets at once, and the rank comes from a 256-byte
// lookup. Upstream does not even compile its magic tables in that configuration
// (attacks.cpp:28) -- it is an algorithm REPLACEMENT above sse41, not an addition.
//
// mcfish ran magics at every tier, which is upstream's pre-AVX2 path. That is 841 KiB
// of attack tables walked at random against upstream's 3 KiB of L1-resident structs,
// on every sliding query -- and the five hottest board sites ask for both sets at
// once. It is the reason the per-node deficit against the oracle tracked the tier:
// near parity at sse41, where both engines use magics, and 8-14% above it.
//
// Below AVX2 this falls back to two magic lookups, which is exactly what upstream's
// non-dual path does.
typedef struct {
    Bitboard bishop;
    Bitboard rook;
} DualAttacks;

DualAttacks both_attacks_bb(Square s, Bitboard occupied);

// Test whether S1, S2 and S3 are collinear. Used to keep a pinned piece on its
// pin ray, which is the whole legality test for a non-king mover.
static inline bool aligned(Square s1, Square s2, Square s3) {
    return (LineBB[s1][s2] & ((Bitboard) 1 << s3)) != 0;
}

#endif  // MCFISH_ATTACKS_H
