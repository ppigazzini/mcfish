// Expose the bitboard vocabulary: square/file/rank masks, the population and scan
// intrinsics, and the set shifts. The attack TABLES live in attacks.h — this
// header is the std-only leaf they are built on, so it must not depend on them.
//
// A Bitboard is a set of squares, bit `s` standing for square `s`. Every function
// here is total over its declared domain except `lsb`/`msb`/`pop_lsb`, which are
// UNDEFINED on an empty board — the underlying bit-scan builtins are. Every caller
// must establish `b != 0` first, which is why the tree writes `while (b) { ...
// pop_lsb(&b) }` and never a do/while.
//
// Upstream: bitboard.h:173 (Magic::index and the attack bitboards).

#ifndef MCFISH_BITBOARD_H
#define MCFISH_BITBOARD_H

#include "types.h"

#include <stdbit.h>

// Build SquareBB. Call once, before attacks_init.
void bitboards_init(void);

extern Bitboard SquareBB[SQUARE_NB];

enum : Bitboard {
    FILE_A_BB = 0x0101010101010101ULL,
    RANK_1_BB = 0x00000000000000FFULL,
    ALL_SQUARES_BB = ~0ULL,
};

static inline Bitboard square_bb(Square s) { return 1ULL << s; }
static inline Bitboard file_bb(int f) { return FILE_A_BB << f; }
static inline Bitboard rank_bb(int r) { return RANK_1_BB << (8 * r); }

static inline bool bb_test(Bitboard b, Square s) { return (b & square_bb(s)) != 0; }
static inline bool bb_more_than_one(Bitboard b) { return (b & (b - 1)) != 0; }

static inline int popcount_bb(Bitboard b) { return (int) stdc_count_ones(b); }

// `lsb` and `msb` keep the compiler builtins, and that is a measured decision
// rather than an oversight. C23's `stdc_trailing_zeros` and `stdc_leading_zeros`
// are DEFINED at zero -- they return the operand width -- where `__builtin_ctzll`
// and `__builtin_clzll` are undefined there. Buying that guarantee costs one
// instruction per call at the sse41 baseline, because without BMI1 the compiler
// must seed the result register before `bsf`/`bsr`:
//
//   __builtin_ctzll      tzcnt %rdi,%rax
//   stdc_trailing_zeros  mov $0x40,%eax ; tzcnt %rdi,%rax
//
// At avx2 and above the two are identical, because BMI1's real `tzcnt` already
// returns 64 for a zero operand. But `pop_lsb` is the hottest line in the engine
// and sse41 is the tier the bench anchor is derived at, so this would pay per
// call, at the primary tier, for a case both functions' preconditions already
// exclude: every caller tests the board non-empty first. `popcount_bb` above is
// the opposite case -- `stdc_count_ones` emits the same lone `popcnt` at every
// tier -- so it takes the standard spelling and these two do not.
static inline Square lsb(Bitboard b) { return (Square) __builtin_ctzll(b); }

static inline Square msb(Bitboard b) { return (Square) (63 ^ __builtin_clzll(b)); }

static inline Square pop_lsb(Bitboard *b) {
    const Square s = lsb(*b);
    *b &= *b - 1;
    return s;
}

// Shift the whole set one step in D, dropping the bits that wrap off the board.
static inline Bitboard shift_bb(Direction d, Bitboard b) {
    switch (d) {
    case NORTH :
        return b << 8;
    case SOUTH :
        return b >> 8;
    case EAST :
        return (b & ~file_bb(7)) << 1;
    case WEST :
        return (b & ~file_bb(0)) >> 1;
    case NORTH_EAST :
        return (b & ~file_bb(7)) << 9;
    case NORTH_WEST :
        return (b & ~file_bb(0)) << 7;
    case SOUTH_EAST :
        return (b & ~file_bb(7)) >> 7;
    case SOUTH_WEST :
        return (b & ~file_bb(0)) >> 9;
    }
    return 0;
}

static inline Bitboard pawn_attacks_bb(Color c, Bitboard b) {
    return c == WHITE ? shift_bb(NORTH_WEST, b) | shift_bb(NORTH_EAST, b)
                      : shift_bb(SOUTH_WEST, b) | shift_bb(SOUTH_EAST, b);
}

#endif  // MCFISH_BITBOARD_H
