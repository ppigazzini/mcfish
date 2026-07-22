// Implement the HalfKAv2_hm and full_threats feature indexers.
//
// C23 has no compile-time evaluation for tables of this shape, so every table here is
// built once by nnue_feature_init (docs/08-idiomatic-c.md, "comptime becomes ... a table
// filled at startup"). The values do not depend on when they are built — the generators
// are the same arithmetic over the same constants.

#include "nnue_feature.h"

#include "nnue_feature_bb.h"

enum {
    W_PAWN = 1,
    W_KNIGHT = 2,
    W_BISHOP = 3,
    W_ROOK = 4,
    W_QUEEN = 5,
    W_KING = 6,
    B_PAWN = 9,
    B_KNIGHT = 10,
    B_BISHOP = 11,
    B_ROOK = 12,
    B_QUEEN = 13,
    B_KING = 14,
    SQ_A2 = 8,
    SQ_H7 = 55,
};

static const uint8_t AllPieces[12] = { W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                                       B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING };

// --- the constant tables ----------------------------------------------------------

static const uint32_t PieceSquareIndex[2][16] = {
    { 0, 0, 128, 256, 384, 512, 640, 0, 0, 64, 192, 320, 448, 576, 640, 0 },
    { 0, 64, 192, 320, 448, 576, 640, 0, 0, 0, 128, 256, 384, 512, 640, 0 },
};

static const uint32_t KingBuckets[64] = {
    28 * NNUE_PS_NB, 29 * NNUE_PS_NB, 30 * NNUE_PS_NB, 31 * NNUE_PS_NB, 31 * NNUE_PS_NB,
    30 * NNUE_PS_NB, 29 * NNUE_PS_NB, 28 * NNUE_PS_NB, 24 * NNUE_PS_NB, 25 * NNUE_PS_NB,
    26 * NNUE_PS_NB, 27 * NNUE_PS_NB, 27 * NNUE_PS_NB, 26 * NNUE_PS_NB, 25 * NNUE_PS_NB,
    24 * NNUE_PS_NB, 20 * NNUE_PS_NB, 21 * NNUE_PS_NB, 22 * NNUE_PS_NB, 23 * NNUE_PS_NB,
    23 * NNUE_PS_NB, 22 * NNUE_PS_NB, 21 * NNUE_PS_NB, 20 * NNUE_PS_NB, 16 * NNUE_PS_NB,
    17 * NNUE_PS_NB, 18 * NNUE_PS_NB, 19 * NNUE_PS_NB, 19 * NNUE_PS_NB, 18 * NNUE_PS_NB,
    17 * NNUE_PS_NB, 16 * NNUE_PS_NB, 12 * NNUE_PS_NB, 13 * NNUE_PS_NB, 14 * NNUE_PS_NB,
    15 * NNUE_PS_NB, 15 * NNUE_PS_NB, 14 * NNUE_PS_NB, 13 * NNUE_PS_NB, 12 * NNUE_PS_NB,
    8 * NNUE_PS_NB,  9 * NNUE_PS_NB,  10 * NNUE_PS_NB, 11 * NNUE_PS_NB, 11 * NNUE_PS_NB,
    10 * NNUE_PS_NB, 9 * NNUE_PS_NB,  8 * NNUE_PS_NB,  4 * NNUE_PS_NB,  5 * NNUE_PS_NB,
    6 * NNUE_PS_NB,  7 * NNUE_PS_NB,  7 * NNUE_PS_NB,  6 * NNUE_PS_NB,  5 * NNUE_PS_NB,
    4 * NNUE_PS_NB,  0 * NNUE_PS_NB,  1 * NNUE_PS_NB,  2 * NNUE_PS_NB,  3 * NNUE_PS_NB,
    3 * NNUE_PS_NB,  2 * NNUE_PS_NB,  1 * NNUE_PS_NB,  0 * NNUE_PS_NB,
};

// Mirror the file when the king stands on the queen side, so the bucketed feature space
// only has to cover half the board. The half tables and the full tables disagree on which
// half is mirrored, which is upstream's convention, not a transcription slip.
static const uint32_t OrientTblHalf[64] = {
    7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0,
    7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0,
};

static const int8_t OrientTblFull[64] = {
    0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7,
    0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7, 0, 0, 0, 0, 7, 7, 7, 7,
};

static const int32_t NumValidTargets[16] = { 0, 6, 10, 8, 8, 10, 0, 0, 0, 6, 10, 8, 8, 10, 0, 0 };

// Map (attacker type, attacked type) to its slot, -1 for a pair the feature set excludes.
static const int32_t FullMap[6][6] = {
    { 0, 1, -1, 2, -1, -1 }, { 0, 1, 2, 3, 4, -1 }, { 0, 1, 2, 3, -1, -1 },
    { 0, 1, 2, 3, -1, -1 },  { 0, 1, 2, 3, 4, -1 }, { -1, -1, -1, -1, -1, -1 },
};

// --- the tables nnue_feature_init builds ------------------------------------------

typedef struct {
    uint32_t cumulative_piece_offset;
    uint32_t cumulative_offset;
} HelperOffsets;

static HelperOffsets HelperOffsetsTbl[16];
static uint32_t Offsets[16][64];
static uint32_t IndexLut1[16][16][2];
static uint8_t IndexLut2[16][64][64];

static void init_threat_offsets(void) {
    uint32_t cumulative_offset = 0;
    for (unsigned piece_index = 0; piece_index < 12; piece_index++) {
        const uint8_t piece = AllPieces[piece_index];
        uint32_t cumulative_piece_offset = 0;
        for (unsigned from = 0; from < 64; from++) {
            Offsets[piece][from] = cumulative_piece_offset;
            if (nnue_bb_type_of(piece) != NNUE_BB_PAWN) {
                cumulative_piece_offset +=
                  nnue_bb_popcount(nnue_bb_pseudo_attacks(nnue_bb_type_of(piece), from));
            } else if (from >= SQ_A2 && from <= SQ_H7) {
                const uint64_t attacks = piece < 8
                                         ? nnue_bb_pawn_push_or_attacks(NNUE_BB_WHITE, from)
                                         : nnue_bb_pawn_push_or_attacks(NNUE_BB_BLACK, from);
                cumulative_piece_offset += nnue_bb_popcount(attacks);
            }
        }
        HelperOffsetsTbl[piece].cumulative_piece_offset = cumulative_piece_offset;
        HelperOffsetsTbl[piece].cumulative_offset = cumulative_offset;
        cumulative_offset += (uint32_t) NumValidTargets[piece] * cumulative_piece_offset;
    }
}

static void init_index_luts(void) {
    for (unsigned attacker_idx = 0; attacker_idx < 12; attacker_idx++) {
        const uint8_t attacker = AllPieces[attacker_idx];
        for (unsigned attacked_idx = 0; attacked_idx < 12; attacked_idx++) {
            const uint8_t attacked = AllPieces[attacked_idx];
            const bool enemy = (attacker ^ attacked) == 8;
            const uint8_t attacker_type = nnue_bb_type_of(attacker);
            const uint8_t attacked_type = nnue_bb_type_of(attacked);
            const int32_t map_value = FullMap[attacker_type - 1][attacked_type - 1];
            const bool semi_excluded =
              attacker_type == attacked_type && (enemy || attacker_type != NNUE_BB_PAWN);
            if (map_value < 0) {
                IndexLut1[attacker][attacked][0] = NNUE_FULL_DIMENSIONS;
                IndexLut1[attacker][attacked][1] = NNUE_FULL_DIMENSIONS;
                continue;
            }

            const uint32_t feature_slot =
              (uint32_t) (nnue_bb_color_of(attacked) * (NumValidTargets[attacker] / 2) + map_value);
            const uint32_t feature =
              HelperOffsetsTbl[attacker].cumulative_offset
              + feature_slot * HelperOffsetsTbl[attacker].cumulative_piece_offset;

            IndexLut1[attacker][attacked][0] = feature;
            IndexLut1[attacker][attacked][1] = semi_excluded ? NNUE_FULL_DIMENSIONS : feature;
        }
    }
}

static void init_index_lut2(void) {
    uint8_t knight[64][64];
    uint8_t bishop[64][64];
    uint8_t rook[64][64];
    uint8_t queen[64][64];
    uint8_t king[64][64];

    nnue_bb_make_piece_indices_type(NNUE_BB_KNIGHT, knight);
    nnue_bb_make_piece_indices_type(NNUE_BB_BISHOP, bishop);
    nnue_bb_make_piece_indices_type(NNUE_BB_ROOK, rook);
    nnue_bb_make_piece_indices_type(NNUE_BB_QUEEN, queen);
    nnue_bb_make_piece_indices_type(NNUE_BB_KING, king);

    nnue_bb_make_piece_indices_pawn(W_PAWN, IndexLut2[W_PAWN]);
    nnue_bb_make_piece_indices_pawn(B_PAWN, IndexLut2[B_PAWN]);
    __builtin_memcpy(IndexLut2[W_KNIGHT], knight, sizeof knight);
    __builtin_memcpy(IndexLut2[B_KNIGHT], knight, sizeof knight);
    __builtin_memcpy(IndexLut2[W_BISHOP], bishop, sizeof bishop);
    __builtin_memcpy(IndexLut2[B_BISHOP], bishop, sizeof bishop);
    __builtin_memcpy(IndexLut2[W_ROOK], rook, sizeof rook);
    __builtin_memcpy(IndexLut2[B_ROOK], rook, sizeof rook);
    __builtin_memcpy(IndexLut2[W_QUEEN], queen, sizeof queen);
    __builtin_memcpy(IndexLut2[B_QUEEN], queen, sizeof queen);
    __builtin_memcpy(IndexLut2[W_KING], king, sizeof king);
    __builtin_memcpy(IndexLut2[B_KING], king, sizeof king);
}

void nnue_feature_init(void) {
    init_threat_offsets();
    init_index_luts();
    init_index_lut2();
}

// --- HalfKAv2_hm ------------------------------------------------------------------

uint32_t
nnue_half_make_index(uint8_t perspective, uint8_t square, uint8_t piece, uint8_t king_square) {
    const uint32_t flip = 56u * perspective;
    return ((uint32_t) square ^ OrientTblHalf[king_square] ^ flip)
         + PieceSquareIndex[perspective][piece]
         + KingBuckets[king_square ^ (uint8_t) (perspective * 56)];
}

// --- full_threats -----------------------------------------------------------------

uint32_t nnue_full_make_index(uint8_t perspective,
                              uint8_t attacker,
                              uint8_t from_sq,
                              uint8_t to_sq,
                              uint8_t attacked,
                              uint8_t king_square) {
    const int32_t orientation = (int32_t) OrientTblFull[king_square] ^ (int32_t) (56 * perspective);
    const uint8_t orient = (uint8_t) orientation;
    const uint8_t from_oriented = (uint8_t) (from_sq ^ orient);
    const uint8_t to_oriented = (uint8_t) (to_sq ^ orient);
    const uint8_t swap = (uint8_t) (8 * perspective);
    const uint8_t attacker_oriented = (uint8_t) (attacker ^ swap);
    const uint8_t attacked_oriented = (uint8_t) (attacked ^ swap);
    const unsigned less = from_oriented < to_oriented ? 1u : 0u;
    return IndexLut1[attacker_oriented][attacked_oriented][less]
         + Offsets[attacker_oriented][from_oriented]
         + IndexLut2[attacker_oriented][from_oriented][to_oriented];
}

// Append only when the pair is inside the feature space: an excluded pair indexes past
// NNUE_FULL_DIMENSIONS, which is the exclusion mechanism (see the header).
static void append_full_active_index(NnueFullAppendResult *result,
                                     uint8_t perspective,
                                     uint8_t attacker,
                                     unsigned from_sq,
                                     unsigned to_sq,
                                     uint8_t attacked,
                                     uint8_t king_square) {
    const uint32_t index = nnue_full_make_index(perspective, attacker, (uint8_t) from_sq,
                                                (uint8_t) to_sq, attacked, king_square);
    if (index < NNUE_FULL_DIMENSIONS) {
        result->indices[result->len] = index;
        result->len += 1;
    }
}

static void process_pawn_attacks(NnueFullAppendResult *result,
                                 uint8_t perspective,
                                 uint8_t attacker,
                                 uint8_t king_square,
                                 const uint8_t *pieces,
                                 uint64_t attacks,
                                 int8_t attack_dir) {
    uint64_t pending = attacks;
    while (pending != 0) {
        const unsigned to = nnue_bb_pop_lsb(&pending);
        const unsigned from = (unsigned) ((int32_t) to - (int32_t) attack_dir);
        append_full_active_index(result, perspective, attacker, from, to, pieces[to], king_square);
    }
}

static void append_active_pawn_threats(NnueFullAppendResult *result,
                                       const uint8_t *pieces,
                                       uint64_t occupied,
                                       uint64_t pawns,
                                       uint64_t color_pawns,
                                       uint8_t perspective,
                                       uint8_t color,
                                       uint8_t king_square) {
    const uint8_t attacker = nnue_bb_make_piece(color, NNUE_BB_PAWN);
    const uint64_t pushers = nnue_bb_pawn_single_push((uint8_t) (color ^ 1), pawns) & color_pawns;

    if (color == NNUE_BB_WHITE) {
        process_pawn_attacks(result, perspective, attacker, king_square, pieces,
                             nnue_bb_shift(NNUE_BB_NORTH_EAST, color_pawns) & occupied,
                             NNUE_BB_NORTH_EAST);
        process_pawn_attacks(result, perspective, attacker, king_square, pieces,
                             nnue_bb_shift(NNUE_BB_NORTH_WEST, color_pawns) & occupied,
                             NNUE_BB_NORTH_WEST);
        process_pawn_attacks(result, perspective, attacker, king_square, pieces,
                             nnue_bb_shift(NNUE_BB_NORTH, pushers), NNUE_BB_NORTH);
    } else {
        process_pawn_attacks(result, perspective, attacker, king_square, pieces,
                             nnue_bb_shift(NNUE_BB_SOUTH_WEST, color_pawns) & occupied,
                             NNUE_BB_SOUTH_WEST);
        process_pawn_attacks(result, perspective, attacker, king_square, pieces,
                             nnue_bb_shift(NNUE_BB_SOUTH_EAST, color_pawns) & occupied,
                             NNUE_BB_SOUTH_EAST);
        process_pawn_attacks(result, perspective, attacker, king_square, pieces,
                             nnue_bb_shift(NNUE_BB_SOUTH, pushers), NNUE_BB_SOUTH);
    }
}

void nnue_full_append_active(uint8_t perspective,
                             uint8_t king_square,
                             const uint8_t *board,
                             const uint64_t *by_type,
                             const uint64_t *by_color,
                             NnueFullAppendResult *out) {
    // Read the piece sets from the Position's cached bitboards (by_type index 0 is the
    // occupancy, and by_color[c] & by_type[pt] is the exact (colour, type) set --
    // identical to scanning board[] because the nnue_bb piece/square encoding matches
    // the engine's) instead of rebuilding ~10 bitboards by 64-square scans per refresh.
    const uint64_t occupied = by_type[0];
    const uint64_t pawns = by_type[NNUE_BB_PAWN];

    out->len = 0;
    for (uint8_t color_index = 0; color_index < 2; color_index++) {
        const uint8_t color = (uint8_t) (perspective ^ color_index);
        const uint64_t color_pawns = by_color[color] & by_type[NNUE_BB_PAWN];
        append_active_pawn_threats(out, board, occupied, pawns, color_pawns, perspective, color,
                                   king_square);

        for (uint8_t piece_type = NNUE_BB_KNIGHT; piece_type < NNUE_BB_KING; piece_type++) {
            const uint8_t attacker = nnue_bb_make_piece(color, piece_type);
            uint64_t attackers = by_color[color] & by_type[piece_type];
            while (attackers != 0) {
                const unsigned from = nnue_bb_pop_lsb(&attackers);
                uint64_t attacks = nnue_bb_attacks(piece_type, from, occupied) & occupied;
                while (attacks != 0) {
                    const unsigned to = nnue_bb_pop_lsb(&attacks);
                    append_full_active_index(out, perspective, attacker, from, to, board[to],
                                             king_square);
                }
            }
        }
    }
}
