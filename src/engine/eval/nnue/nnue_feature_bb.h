// Own the bitboard math the NNUE feature indexers are built from: the square/piece/
// attack helpers plus the two per-piece index-table generators.
//
// The INVARIANT is self-containment. These are a deliberate re-implementation of
// `bitboard.h` / `attacks.h`, not a call into them: zfish evaluates them at comptime to
// build the feature-index tables, and the C port builds those tables at startup instead
// (docs/06-idiomatic-c.md, "comptime becomes ... a runtime-built table"). Depending on
// `attacks_init` here would make the feature tables depend on an init order nothing
// states. Every function is total over its declared domain and reads no engine state.
//
// Pieces and squares are plain `uint8_t` here, in upstream's NNUE encoding
// (`color << 3 | type`, squares A1..H8), because the feature indexer's tables are indexed
// by the raw value and never by the board zone's enums.
//
// Ported from zfish `engine/eval/nnue_feature_bb.zig` against the upstream golden
// `nnue/features/half_ka_v2_hm.cpp` and `nnue/features/full_threats.cpp`.

#ifndef CCFISH_NNUE_FEATURE_BB_H
#define CCFISH_NNUE_FEATURE_BB_H

#include <stdint.h>

enum : uint8_t {
    NNUE_BB_WHITE = 0,
    NNUE_BB_BLACK = 1,
    NNUE_BB_NO_PIECE = 0,
    NNUE_BB_PAWN = 1,
    NNUE_BB_KNIGHT = 2,
    NNUE_BB_BISHOP = 3,
    NNUE_BB_ROOK = 4,
    NNUE_BB_QUEEN = 5,
    NNUE_BB_KING = 6,
};

enum : uint64_t {
    NNUE_BB_FILE_A = 0x0101010101010101ULL,
    NNUE_BB_FILE_H = 0x0101010101010101ULL << 7,
};

// Name the eight step vectors as upstream does. Signed, because half of them are.
enum : int8_t {
    NNUE_BB_NORTH = 8,
    NNUE_BB_EAST = 1,
    NNUE_BB_SOUTH = -8,
    NNUE_BB_WEST = -1,
    NNUE_BB_NORTH_EAST = 9,
    NNUE_BB_NORTH_WEST = 7,
    NNUE_BB_SOUTH_EAST = -7,
    NNUE_BB_SOUTH_WEST = -9,
};

enum { NNUE_BB_SQUARE_COUNT = 64 };

static inline uint8_t nnue_bb_make_piece(uint8_t color, uint8_t piece_type) {
    return (uint8_t) ((uint8_t) (color << 3) + piece_type);
}

static inline uint8_t nnue_bb_type_of(uint8_t piece) { return (uint8_t) (piece & 7); }

static inline uint8_t nnue_bb_color_of(uint8_t piece) { return (uint8_t) (piece >> 3); }

static inline uint64_t nnue_bb_square(unsigned square) { return (uint64_t) 1 << square; }

static inline uint8_t nnue_bb_popcount(uint64_t bitboard) {
    return (uint8_t) __builtin_popcountll(bitboard);
}

// Pop and return the least significant square. UNDEFINED on an empty board, as the
// underlying bit-scan builtin is; every caller establishes `*bitboard != 0` first.
static inline unsigned nnue_bb_pop_lsb(uint64_t *bitboard) {
    const unsigned square = (unsigned) __builtin_ctzll(*bitboard);
    *bitboard &= *bitboard - 1;
    return square;
}

// Shift the whole set one step in DIR, dropping the bits that wrap off the board.
uint64_t nnue_bb_shift(int8_t dir, uint64_t bitboard);

// Return COLOR's single push together with its two attacks from SQUARE.
uint64_t nnue_bb_pawn_push_or_attacks(uint8_t color, unsigned square);

// Return the destination of STEP from SQUARE, or 0 when the step leaves the board.
// Guards on file distance, not on index range: a wrapped index is in range and
// geometrically wrong.
uint64_t nnue_bb_safe_destination(unsigned square, int8_t step);

uint64_t nnue_bb_sliding_attack(uint8_t piece_type, unsigned square, uint64_t occupied);
uint64_t nnue_bb_knight_attack(unsigned square);
uint64_t nnue_bb_king_attack(unsigned square);

// Return PIECE_TYPE's attack set from SQUARE given OCCUPIED. Knights ignore OCCUPIED;
// pawns and kings return 0, which is what the threat indexer's callers rely on.
uint64_t nnue_bb_attacks(uint8_t piece_type, unsigned square, uint64_t occupied);

// Return PIECE_TYPE's attack set from SQUARE on an empty board.
uint64_t nnue_bb_pseudo_attacks(uint8_t piece_type, unsigned square);

// Fold a 64-entry piece array into a bitboard. PIECES must have 64 entries.
uint64_t nnue_bb_pieces_of_exact(const uint8_t *pieces, uint8_t wanted);
uint64_t nnue_bb_pieces_of_type(const uint8_t *pieces, uint8_t wanted_type);
uint64_t nnue_bb_occupied_from_pieces(const uint8_t *pieces);

static inline uint64_t nnue_bb_pawn_single_push(uint8_t color, uint64_t bitboard) {
    return color == NNUE_BB_WHITE ? nnue_bb_shift(NNUE_BB_NORTH, bitboard)
                                  : nnue_bb_shift(NNUE_BB_SOUTH, bitboard);
}

// Fill OUT[from][to] with the rank of TO inside PIECE_TYPE's pseudo-attack set from
// FROM — the ordinal the threat feature index is built on.
void nnue_bb_make_piece_indices_type(uint8_t piece_type, uint8_t out[64][64]);

// As above, for PIECE's pawn push-or-attack set.
void nnue_bb_make_piece_indices_pawn(uint8_t piece, uint8_t out[64][64]);

#endif  // CCFISH_NNUE_FEATURE_BB_H
