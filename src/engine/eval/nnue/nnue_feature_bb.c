// Implement the NNUE feature-index bitboard math. See nnue_feature_bb.h for the
// self-containment invariant.

#include "nnue_feature_bb.h"

static const int8_t RookDirs[4] = { NNUE_BB_NORTH, NNUE_BB_SOUTH, NNUE_BB_EAST, NNUE_BB_WEST };

static const int8_t BishopDirs[4] = { NNUE_BB_NORTH_EAST, NNUE_BB_SOUTH_EAST, NNUE_BB_SOUTH_WEST,
                                      NNUE_BB_NORTH_WEST };

static const int8_t QueenDirs[8] = { NNUE_BB_NORTH,      NNUE_BB_SOUTH,      NNUE_BB_EAST,
                                     NNUE_BB_WEST,       NNUE_BB_NORTH_EAST, NNUE_BB_SOUTH_EAST,
                                     NNUE_BB_SOUTH_WEST, NNUE_BB_NORTH_WEST };

static const int8_t KnightSteps[8] = { -17, -15, -10, -6, 6, 10, 15, 17 };

static const int8_t KingSteps[8] = { -9, -8, -7, -1, 1, 7, 8, 9 };

uint64_t nnue_bb_shift(int8_t dir, uint64_t bitboard) {
    switch (dir) {
    case NNUE_BB_NORTH :
        return bitboard << 8;
    case NNUE_BB_SOUTH :
        return bitboard >> 8;
    case NNUE_BB_EAST :
        return (bitboard & ~(uint64_t) NNUE_BB_FILE_H) << 1;
    case NNUE_BB_WEST :
        return (bitboard & ~(uint64_t) NNUE_BB_FILE_A) >> 1;
    case NNUE_BB_NORTH_EAST :
        return (bitboard & ~(uint64_t) NNUE_BB_FILE_H) << 9;
    case NNUE_BB_NORTH_WEST :
        return (bitboard & ~(uint64_t) NNUE_BB_FILE_A) << 7;
    case NNUE_BB_SOUTH_EAST :
        return (bitboard & ~(uint64_t) NNUE_BB_FILE_H) >> 7;
    case NNUE_BB_SOUTH_WEST :
        return (bitboard & ~(uint64_t) NNUE_BB_FILE_A) >> 9;
    default :
        return 0;
    }
}

uint64_t nnue_bb_pawn_push_or_attacks(uint8_t color, unsigned square) {
    const uint64_t one = nnue_bb_square(square);
    return color == NNUE_BB_WHITE
           ? nnue_bb_shift(NNUE_BB_NORTH, one) | nnue_bb_shift(NNUE_BB_NORTH_WEST, one)
               | nnue_bb_shift(NNUE_BB_NORTH_EAST, one)
           : nnue_bb_shift(NNUE_BB_SOUTH, one) | nnue_bb_shift(NNUE_BB_SOUTH_WEST, one)
               | nnue_bb_shift(NNUE_BB_SOUTH_EAST, one);
}

uint64_t nnue_bb_safe_destination(unsigned square, int8_t step) {
    const int32_t target = (int32_t) square + step;
    if (target < 0 || target >= 64) {
        return 0;
    }
    const unsigned from_file = square % 8;
    const unsigned to_file = (unsigned) target % 8;
    const unsigned diff = from_file > to_file ? from_file - to_file : to_file - from_file;
    if (diff > 2) {
        return 0;
    }
    return nnue_bb_square((unsigned) target);
}

uint64_t nnue_bb_sliding_attack(uint8_t piece_type, unsigned square, uint64_t occupied) {
    const int8_t *dirs;
    unsigned dir_count;
    switch (piece_type) {
    case NNUE_BB_BISHOP :
        dirs = BishopDirs;
        dir_count = 4;
        break;
    case NNUE_BB_ROOK :
        dirs = RookDirs;
        dir_count = 4;
        break;
    case NNUE_BB_QUEEN :
        dirs = QueenDirs;
        dir_count = 8;
        break;
    default :
        return 0;
    }

    uint64_t attacks = 0;
    for (unsigned i = 0; i < dir_count; i++) {
        unsigned current = square;
        for (;;) {
            const uint64_t dest = nnue_bb_safe_destination(current, dirs[i]);
            if (dest == 0) {
                break;
            }
            attacks |= dest;
            current = (unsigned) __builtin_ctzll(dest);
            if ((occupied & dest) != 0) {
                break;
            }
        }
    }
    return attacks;
}

uint64_t nnue_bb_knight_attack(unsigned square) {
    uint64_t bitboard = 0;
    for (unsigned i = 0; i < 8; i++) {
        bitboard |= nnue_bb_safe_destination(square, KnightSteps[i]);
    }
    return bitboard;
}

uint64_t nnue_bb_king_attack(unsigned square) {
    uint64_t bitboard = 0;
    for (unsigned i = 0; i < 8; i++) {
        bitboard |= nnue_bb_safe_destination(square, KingSteps[i]);
    }
    return bitboard;
}

uint64_t nnue_bb_attacks(uint8_t piece_type, unsigned square, uint64_t occupied) {
    switch (piece_type) {
    case NNUE_BB_KNIGHT :
        return nnue_bb_knight_attack(square);
    case NNUE_BB_BISHOP :
        return nnue_bb_sliding_attack(NNUE_BB_BISHOP, square, occupied);
    case NNUE_BB_ROOK :
        return nnue_bb_sliding_attack(NNUE_BB_ROOK, square, occupied);
    case NNUE_BB_QUEEN :
        return nnue_bb_sliding_attack(NNUE_BB_QUEEN, square, occupied);
    default :
        return 0;
    }
}

uint64_t nnue_bb_pseudo_attacks(uint8_t piece_type, unsigned square) {
    switch (piece_type) {
    case NNUE_BB_KNIGHT :
        return nnue_bb_knight_attack(square);
    case NNUE_BB_BISHOP :
        return nnue_bb_sliding_attack(NNUE_BB_BISHOP, square, 0);
    case NNUE_BB_ROOK :
        return nnue_bb_sliding_attack(NNUE_BB_ROOK, square, 0);
    case NNUE_BB_QUEEN :
        return nnue_bb_sliding_attack(NNUE_BB_QUEEN, square, 0);
    case NNUE_BB_KING :
        return nnue_bb_king_attack(square);
    default :
        return 0;
    }
}

void nnue_bb_make_piece_indices_type(uint8_t piece_type, uint8_t out[64][64]) {
    for (unsigned from = 0; from < 64; from++) {
        const uint64_t attacks = nnue_bb_pseudo_attacks(piece_type, from);
        for (unsigned to = 0; to < 64; to++) {
            out[from][to] = nnue_bb_popcount((nnue_bb_square(to) - 1) & attacks);
        }
    }
}

void nnue_bb_make_piece_indices_pawn(uint8_t piece, uint8_t out[64][64]) {
    const uint8_t color = nnue_bb_color_of(piece);
    for (unsigned from = 0; from < 64; from++) {
        const uint64_t attacks = nnue_bb_pawn_push_or_attacks(color, from);
        for (unsigned to = 0; to < 64; to++) {
            out[from][to] = nnue_bb_popcount((nnue_bb_square(to) - 1) & attacks);
        }
    }
}
