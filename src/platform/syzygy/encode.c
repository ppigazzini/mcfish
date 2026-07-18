#include "encode.h"

#include <string.h>

int32_t MapB1H1H7[64];
int32_t MapA1D1D4[64];
int32_t MapKK[10][64];
int32_t Binomial[6][64];
int32_t MapPawns[64];
int32_t LeadPawnIdx[6][64];
int32_t LeadPawnsSize[6][4];
int32_t KkCount = 0;

static inline unsigned rank_of_sq(unsigned sq) { return sq >> 3; }
static inline unsigned file_of_sq(unsigned sq) { return sq & 7; }
static inline unsigned make_sq(unsigned f, unsigned r) { return r * 8 + f; }
static inline unsigned flip_file_sq(unsigned sq) { return sq ^ 7; }

// Detect a king "touch": S2 equals S1 or is a king move from it (Chebyshev <= 1).
static bool king_touch(unsigned s1, unsigned s2) {
    const int df = (int) file_of_sq(s1) - (int) file_of_sq(s2);
    const int dr = (int) rank_of_sq(s1) - (int) rank_of_sq(s2);
    return (df >= -1 && df <= 1) && (dr >= -1 && dr <= 1);
}

enum : unsigned { SQ_D4_IDX = 27 };  // make_sq(FILE_D, RANK_4)

void encode_init_geometry(void) {
    memset(MapB1H1H7, 0, sizeof MapB1H1H7);
    memset(MapA1D1D4, 0, sizeof MapA1D1D4);
    memset(MapKK, 0, sizeof MapKK);
    memset(Binomial, 0, sizeof Binomial);
    memset(MapPawns, 0, sizeof MapPawns);
    memset(LeadPawnIdx, 0, sizeof LeadPawnIdx);
    memset(LeadPawnsSize, 0, sizeof LeadPawnsSize);

    // Map a square below the a1-h8 diagonal to 0..27.
    int32_t code = 0;
    for (unsigned s = 0; s < 64; ++s) {
        if (off_a1h8(s) < 0) {
            MapB1H1H7[s] = code++;
        }
    }

    // Map a square of the a1-d1-d4 triangle to 0..9, the diagonal squares last.
    unsigned diagonal[4];
    size_t ndiag = 0;
    code = 0;
    for (unsigned s = 0; s <= SQ_D4_IDX; ++s) {
        if (off_a1h8(s) < 0 && file_of_sq(s) <= 3) {
            MapA1D1D4[s] = code++;
        } else if (off_a1h8(s) == 0 && file_of_sq(s) <= 3) {
            diagonal[ndiag++] = s;
        }
    }
    for (size_t i = 0; i < ndiag; ++i) {
        MapA1D1D4[diagonal[i]] = code++;
    }

    // Map the 462 legal placements of two kings, the first in the a1-d1-d4
    // triangle. Assign the both-kings-on-the-diagonal cases last, as upstream does.
    struct {
        size_t idx;
        unsigned s2;
    } both_on_diag[64];
    size_t nboth = 0;
    code = 0;
    for (size_t idx = 0; idx < 10; ++idx) {
        for (unsigned s1 = 0; s1 <= SQ_D4_IDX; ++s1) {
            if (!(MapA1D1D4[s1] == (int32_t) idx && (idx != 0 || s1 == 1))) {  // SQ_B1 == 1
                continue;
            }
            for (unsigned s2 = 0; s2 < 64; ++s2) {
                if (king_touch(s1, s2)) {
                    continue;  // adjacent or coincident kings: illegal
                }
                if (off_a1h8(s1) == 0 && off_a1h8(s2) > 0) {
                    continue;  // first on the diagonal, second above it
                }
                if (off_a1h8(s1) == 0 && off_a1h8(s2) == 0) {
                    both_on_diag[nboth].idx = idx;
                    both_on_diag[nboth].s2 = s2;
                    ++nboth;
                } else {
                    MapKK[idx][s2] = code++;
                }
            }
        }
    }
    for (size_t i = 0; i < nboth; ++i) {
        MapKK[both_on_diag[i].idx][both_on_diag[i].s2] = code++;
    }
    KkCount = code;

    // Fill Binomial[k][n] == C(n, k) by Pascal's rule.
    Binomial[0][0] = 1;
    for (size_t n = 1; n < 64; ++n) {
        for (size_t k = 0; k < 6 && k <= n; ++k) {
            Binomial[k][n] =
              (k > 0 ? Binomial[k - 1][n - 1] : 0) + (k < n ? Binomial[k][n - 1] : 0);
        }
    }

    // Map a2-h7 to 0..47, then build the leading-pawn index for up to 5 pawns.
    int32_t available = 47;
    for (size_t lead_cnt = 1; lead_cnt <= 5; ++lead_cnt) {
        for (unsigned f = 0; f <= 3; ++f) {  // FILE_A .. FILE_D
            int32_t pidx = 0;
            for (unsigned r = 1; r <= 6; ++r) {  // RANK_2 .. RANK_7
                const unsigned sq = make_sq(f, r);
                if (lead_cnt == 1) {
                    MapPawns[sq] = available--;
                    MapPawns[flip_file_sq(sq)] = available--;
                }
                LeadPawnIdx[lead_cnt][sq] = pidx;
                pidx += Binomial[lead_cnt - 1][MapPawns[sq]];
            }
            LeadPawnsSize[lead_cnt][f] = pidx;
        }
    }
}
