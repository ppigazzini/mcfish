// Define the engine-wide value domain: colors, pieces, squares, moves, scores.
//
// Every type here is a fixed-width integer with a named total range. The width is
// load-bearing: `Piece` packs into `Position::board[64]` and `Square` indexes every
// attack table, so widening a type without widening its `*_NB` bound turns a
// bounds check into a silent out-of-range read.

#ifndef MCFISH_TYPES_H
#define MCFISH_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t Bitboard;
typedef uint64_t Key;

enum {
    COLOR_NB = 2,
    PIECE_TYPE_NB = 7,  // NO_PIECE_TYPE + 6 real types
    PIECE_NB = 16,      // 8 per color, sparse: color << 3 | type
    SQUARE_NB = 64,
    FILE_NB = 8,
    RANK_NB = 8,
    MAX_MOVES = 256,  // upper bound on legal moves in any reachable position
    MAX_PLY = 246,
};

typedef enum : uint8_t { WHITE = 0, BLACK = 1 } Color;

typedef enum : uint8_t {
    NO_PIECE_TYPE = 0,
    PAWN = 1,
    KNIGHT = 2,
    BISHOP = 3,
    ROOK = 4,
    QUEEN = 5,
    KING = 6,
    ALL_PIECES = 0,
} PieceType;

// Encode a piece as `color << 3 | type`, leaving 7 and 15 unused. The gap keeps
// the color bit at a fixed position so `color_of` is a shift, not a table.
typedef enum : uint8_t {
    NO_PIECE = 0,
    W_PAWN = 1,
    W_KNIGHT,
    W_BISHOP,
    W_ROOK,
    W_QUEEN,
    W_KING,
    B_PAWN = 9,
    B_KNIGHT,
    B_BISHOP,
    B_ROOK,
    B_QUEEN,
    B_KING,
} Piece;

// Order squares A1..H8 rank-major, so `sq >> 3` is the rank and `sq & 7` the file.
typedef enum : uint8_t {
    SQ_A1,
    SQ_B1,
    SQ_C1,
    SQ_D1,
    SQ_E1,
    SQ_F1,
    SQ_G1,
    SQ_H1,
    SQ_A2,
    SQ_B2,
    SQ_C2,
    SQ_D2,
    SQ_E2,
    SQ_F2,
    SQ_G2,
    SQ_H2,
    SQ_A3,
    SQ_B3,
    SQ_C3,
    SQ_D3,
    SQ_E3,
    SQ_F3,
    SQ_G3,
    SQ_H3,
    SQ_A4,
    SQ_B4,
    SQ_C4,
    SQ_D4,
    SQ_E4,
    SQ_F4,
    SQ_G4,
    SQ_H4,
    SQ_A5,
    SQ_B5,
    SQ_C5,
    SQ_D5,
    SQ_E5,
    SQ_F5,
    SQ_G5,
    SQ_H5,
    SQ_A6,
    SQ_B6,
    SQ_C6,
    SQ_D6,
    SQ_E6,
    SQ_F6,
    SQ_G6,
    SQ_H6,
    SQ_A7,
    SQ_B7,
    SQ_C7,
    SQ_D7,
    SQ_E7,
    SQ_F7,
    SQ_G7,
    SQ_H7,
    SQ_A8,
    SQ_B8,
    SQ_C8,
    SQ_D8,
    SQ_E8,
    SQ_F8,
    SQ_G8,
    SQ_H8,
    SQ_NONE,
} Square;

typedef enum : int8_t {
    NORTH = 8,
    EAST = 1,
    SOUTH = -8,
    WEST = -1,
    NORTH_EAST = 9,
    SOUTH_EAST = -7,
    SOUTH_WEST = -9,
    NORTH_WEST = 7,
} Direction;

// Pack castling rights as one nibble so the whole right set is a single Zobrist index.
typedef enum : uint8_t {
    NO_CASTLING = 0,
    WHITE_OO = 1,
    WHITE_OOO = 2,
    BLACK_OO = 4,
    BLACK_OOO = 8,
    ANY_CASTLING = 15,
} CastlingRights;

typedef enum : uint8_t {
    NORMAL = 0,
    PROMOTION = 1,
    EN_PASSANT = 2,
    CASTLING = 3,
} MoveType;

// Pack a move into 16 bits: `type << 14 | (promo - KNIGHT) << 12 | from << 6 | to`.
// MOVE_NONE and MOVE_NULL are the two encodings with from == to, which no legal
// move can produce, so they never collide with a real move.
typedef uint16_t Move;

enum { MOVE_NONE = 0, MOVE_NULL = 65 };

typedef int32_t Value;

enum : int32_t {
    VALUE_ZERO = 0,
    VALUE_DRAW = 0,
    VALUE_NONE = 32002,
    VALUE_INFINITE = 32001,
    VALUE_MATE = 32000,
    VALUE_MATE_IN_MAX_PLY = VALUE_MATE - MAX_PLY,
    VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY,

    PAWN_VALUE = 208,
    KNIGHT_VALUE = 781,
    BISHOP_VALUE = 825,
    ROOK_VALUE = 1276,
    QUEEN_VALUE = 2538,
};
static inline Color color_of_piece(Piece pc) { return (Color) (pc >> 3); }
static inline PieceType type_of_piece(Piece pc) { return (PieceType) (pc & 7); }
static inline Piece make_piece(Color c, PieceType pt) { return (Piece) ((c << 3) + pt); }
static inline Color flip_color(Color c) { return (Color) (c ^ BLACK); }

static inline int rank_of(Square s) { return s >> 3; }
static inline int file_of(Square s) { return s & 7; }
static inline Square make_square(int f, int r) { return (Square) ((r << 3) + f); }
static inline Square flip_rank(Square s) { return (Square) (s ^ SQ_A8); }

// Relative rank: rank 0 is the side's own back rank, so pawn logic is color-agnostic.
static inline int relative_rank(Color c, Square s) { return rank_of(s) ^ (c * 7); }

// Step S by D. Square and Direction are distinct enum types, so the arithmetic is
// widened to int and re-tagged once here rather than at every call site.
static inline Square sq_add(Square s, Direction d) { return (Square) ((int) s + (int) d); }
static inline Square sq_sub(Square s, Direction d) { return (Square) ((int) s - (int) d); }

static inline Square move_from(Move m) { return (Square) ((m >> 6) & 0x3F); }
static inline Square move_to(Move m) { return (Square) (m & 0x3F); }
static inline MoveType move_type(Move m) { return (MoveType) ((m >> 14) & 3); }
static inline PieceType move_promotion(Move m) { return (PieceType) (((m >> 12) & 3) + KNIGHT); }
static inline bool move_is_ok(Move m) { return move_from(m) != move_to(m); }

static inline Move make_move(Square from, Square to) { return (Move) ((from << 6) + to); }

static inline Move make_move_typed(MoveType t, Square from, Square to, PieceType promo) {
    return (Move) ((t << 14) + ((promo - KNIGHT) << 12) + (from << 6) + to);
}

static inline Value piece_value(PieceType pt) {
    static const Value v[PIECE_TYPE_NB] = {
        0, PAWN_VALUE, KNIGHT_VALUE, BISHOP_VALUE, ROOK_VALUE, QUEEN_VALUE, 0
    };
    return v[pt];
}

// Value of a raw Piece, indexed without the type mask — upstream's
// PieceValue[PIECE_NB] table (types.h), which see_ge indexes straight off
// piece_on(). The colour bit selects a mirrored half, so the `& 7` a
// PieceType lookup would need never happens on that hot path.
static inline Value piece_value_of(Piece pc) {
    static const Value v[PIECE_NB] = {
        0, PAWN_VALUE, KNIGHT_VALUE, BISHOP_VALUE, ROOK_VALUE, QUEEN_VALUE, 0, 0,
        0, PAWN_VALUE, KNIGHT_VALUE, BISHOP_VALUE, ROOK_VALUE, QUEEN_VALUE, 0, 0
    };
    return v[pc];
}

static inline Value mate_in(int ply) { return (Value) (VALUE_MATE - ply); }
static inline Value mated_in(int ply) { return (Value) (ply - VALUE_MATE); }

// Keep SQ_NONE at the NNUE encoding's "no square". DirtyPiece stores raw uint8_t
// squares, and the accumulator tests them against 64 (nnue_feature.h NNUE_SQ_NONE);
// renumbering Square without renumbering that constant silently makes every
// promotion and capture look like a real square.
static_assert(SQ_NONE == 64, "DirtyPiece's no-square sentinel is NNUE's 64");

// Record the HalfKAv2_hm delta of one move: the piece that moved, plus at most one
// removed and one added piece (a capture, a promotion, or castling's rook).
//
// The layout is a CONTRACT, not an implementation detail. `nnue_feature.h`'s
// NnueDirtyPiece is a byte-identical view of this struct, because pos_do_move
// writes its delta straight into the accumulator's arena; that arena stores the
// record at a deliberately unrounded offset and reads `pc` as the first diff byte.
// Keep it all-uint8_t, alignment-free, and in this field order.
//
// Squares are raw uint8_t with 64 for "none"; pieces are `color << 3 | type`, which
// is already Piece's encoding. Golden: Stockfish/src/types.h:296 (DirtyPiece).
typedef struct DirtyPiece {
    uint8_t pc;         // the moving piece; never NO_PIECE for a real move
    uint8_t from;       // the square it left
    uint8_t to;         // the square it landed on, or 64 for a promotion
    uint8_t remove_sq;  // a captured (or castled rook's origin) square, or 64
    uint8_t add_sq;     // a promoted (or castled rook's destination) square, or 64
    uint8_t remove_pc;  // the removed piece; unspecified when remove_sq is 64
    uint8_t add_pc;     // the added piece; unspecified when add_sq is 64
} DirtyPiece;

enum { DIRTY_THREAT_MAX = 96 };

// Pack a DirtyThreat into 32 bits, matching upstream's field offsets exactly — the
// feature indexer decodes this word, so the layout is a contract, not a choice
// (Stockfish/src/types.h:310-313).
enum {
    DIRTY_THREAT_PC_SQ_OFFSET = 0,
    DIRTY_THREAT_THREATENED_SQ_OFFSET = 8,
    DIRTY_THREAT_THREATENED_PC_OFFSET = 16,
    DIRTY_THREAT_PC_OFFSET = 20,
    DIRTY_THREAT_ADD_OFFSET = 31,
};

// Record the full_threats delta of one move, plus the king-square bookkeeping the
// accumulator needs to decide whether the whole feature block must be rebuilt.
//
// This lives in types.h rather than threats.h for the same reason DirtyPiece does:
// position.h must name it, threats.h must name Position, and DirtyThreats depends
// on neither. That is upstream's split (Stockfish/src/types.h:309-345 owns the
// records; position.h owns update_piece_threats).
//
// `us`, `prev_ksq` and `ksq` must stay contiguous and in this order immediately
// after `list_size`: nnue_accumulator.c static-asserts their offsets against the
// arena slot it hands out.
//
// The list is bounded at 96 and never checked. A non-castling move changes at most
// (8 + 16) * 3 + 8 = 80 features and a castling move at most (5 + 1 + 3 + 9) * 2 =
// 36, so 80 bounds it; the remaining 16 slots exist so an unmasked vector store
// near the end of the list stays in bounds (Stockfish/src/types.h:334).
typedef struct DirtyThreats {
    uint32_t list_values[DIRTY_THREAT_MAX];  // the packed DirtyThreat words
    size_t list_size;                        // the live prefix of list_values
    Color us;                                // the side that moved
    Square prev_ksq;                         // us's king square before the move
    Square ksq;                              // us's king square after the move
} DirtyThreats;

static inline uint32_t
dirty_threat_make(bool add, Piece pc, Piece threatened_pc, Square pc_sq, Square threatened_sq) {
    return ((uint32_t) add << DIRTY_THREAT_ADD_OFFSET) | ((uint32_t) pc << DIRTY_THREAT_PC_OFFSET)
         | ((uint32_t) threatened_pc << DIRTY_THREAT_THREATENED_PC_OFFSET)
         | ((uint32_t) threatened_sq << DIRTY_THREAT_THREATENED_SQ_OFFSET)
         | ((uint32_t) pc_sq << DIRTY_THREAT_PC_SQ_OFFSET);
}

static inline Piece dirty_threat_pc(uint32_t d) {
    return (Piece) ((d >> DIRTY_THREAT_PC_OFFSET) & 0xF);
}
static inline Piece dirty_threat_threatened_pc(uint32_t d) {
    return (Piece) ((d >> DIRTY_THREAT_THREATENED_PC_OFFSET) & 0xF);
}
static inline Square dirty_threat_threatened_sq(uint32_t d) {
    return (Square) ((d >> DIRTY_THREAT_THREATENED_SQ_OFFSET) & 0xFF);
}
static inline Square dirty_threat_pc_sq(uint32_t d) {
    return (Square) ((d >> DIRTY_THREAT_PC_SQ_OFFSET) & 0xFF);
}
static inline bool dirty_threat_add(uint32_t d) { return (d >> DIRTY_THREAT_ADD_OFFSET) != 0; }

// Empty the list. The king squares are written by the make/unmake hooks, not here.
static inline void dirty_threats_clear(DirtyThreats *dts) { dts->list_size = 0; }

#endif  // MCFISH_TYPES_H
