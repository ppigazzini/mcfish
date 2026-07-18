#include "bitboard.h"

Bitboard SquareBB[SQUARE_NB];

void bitboards_init(void) {
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        SquareBB[s] = 1ULL << s;
}
