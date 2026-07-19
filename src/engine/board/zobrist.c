#include "zobrist.h"

Key Zobrist_psq[PIECE_NB][SQUARE_NB];
Key Zobrist_enpassant[FILE_NB];
Key Zobrist_castling[16];
Key Zobrist_side;
Key Zobrist_no_pawns;

// Generate the keys from a fixed-seed xorshift64*, never from the host PRNG: the
// bench signature and every golden are functions of these keys, so a key that
// varies per run or per platform silently invalidates all of them.
//
// The multiply wraps. Key is uint64_t, so the wrap is defined; do not widen it or
// reformulate it as a signed product.
static Key next_key(Key *s) {
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

void zobrist_init(void) {
    Key s = 1070372ULL;

    // Draw for the 12 REAL pieces only, skipping the encoding gaps at 7 and 8.
    // Upstream iterates a 12-element Pieces[] (position.cpp:59-60, :123-125);
    // drawing for the gaps too would consume 128 extra PRNG values and shift every
    // key from B_PAWN onward away from upstream's table.
    static const Piece Pieces[12] = { W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                                      B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING };

    for (int i = 0; i < 12; ++i)
        for (Square sq = SQ_A1; sq <= SQ_H8; ++sq)
            Zobrist_psq[Pieces[i]][sq] = next_key(&s);

    // Zero the ranks a pawn can only reach by promoting. A pawn never rests there,
    // so the entry is unreachable in compute_key; upstream zeroes it so the
    // promotion XOR cancels implicitly (position.cpp:126-127).
    for (int f = 0; f < FILE_NB; ++f) {
        Zobrist_psq[W_PAWN][make_square(f, 7)] = 0;
        Zobrist_psq[B_PAWN][make_square(f, 0)] = 0;
    }

    for (int f = 0; f < FILE_NB; ++f)
        Zobrist_enpassant[f] = next_key(&s);

    for (int cr = 0; cr < 16; ++cr)
        Zobrist_castling[cr] = next_key(&s);

    Zobrist_side = next_key(&s);
    Zobrist_no_pawns = next_key(&s);
}
