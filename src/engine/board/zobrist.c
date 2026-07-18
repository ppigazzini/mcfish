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

    // Draw for pc = 1..14 inclusive. Codes 7 and 8 are unused encoding gaps and
    // their keys are never read, but the draws still happen — removing them
    // shifts every key from Zobrist_psq[B_PAWN] onward. See the header.
    for (Piece pc = W_PAWN; pc <= B_KING; ++pc)
        for (Square sq = SQ_A1; sq <= SQ_H8; ++sq)
            Zobrist_psq[pc][sq] = next_key(&s);

    for (int f = 0; f < FILE_NB; ++f)
        Zobrist_enpassant[f] = next_key(&s);

    for (int cr = 0; cr < 16; ++cr)
        Zobrist_castling[cr] = next_key(&s);

    Zobrist_side = next_key(&s);
    Zobrist_no_pawns = next_key(&s);
}
