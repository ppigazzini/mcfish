// Own the Syzygy position-to-index geometry: the tables upstream's
// `Tablebases::init` precomputes and `do_probe_table` indexes through — the
// binomial coefficients, the king-pair map (462 legal king placements), the
// a1-d1-d4 and below-a1h8 square maps, and the leading-pawn encoding.
//
// Treat this as pure board geometry: computed once by `encode_init_geometry`,
// no I/O and no engine types. Every table reads as zero until that call, so the
// registry builds the geometry before it registers a table.
//
// Mirror upstream `syzygy/tbprobe.cpp:1397` (Tablebases::init).

#ifndef MCFISH_SYZYGY_ENCODE_H
#define MCFISH_SYZYGY_ENCODE_H

#include <stddef.h>
#include <stdint.h>

// Number squares as upstream does: A1 = 0 .. H8 = 63, rank = sq >> 3, file = sq & 7.
extern int32_t MapB1H1H7[64];
extern int32_t MapA1D1D4[64];
extern int32_t MapKK[10][64];
extern int32_t Binomial[6][64];  // [k][n] == C(n, k)
extern int32_t MapPawns[64];
extern int32_t LeadPawnIdx[6][64];
extern int32_t LeadPawnsSize[6][4];
extern int32_t KkCount;  // legal king-pair encodings assigned; 462 once built

// Build every table above. Idempotent; call before any probe.
void encode_init_geometry(void);

// Report rank - file: negative below the a1-h8 diagonal, zero on it, positive above.
static inline int32_t off_a1h8(unsigned sq) { return (int32_t) (sq >> 3) - (int32_t) (sq & 7); }

// Report the distance from F to the nearer edge file, which is the table's file
// index for a leading pawn.
static inline size_t edge_distance(unsigned f) { return f < 7u - f ? f : 7u - f; }

// Read the geometry tables through a range check. A well-formed table never
// indexes outside them, so an out-of-range read means a corrupt file: yield 0 and
// let the caller's probe come out unusable rather than read past the array.
static inline int32_t binomial_at(int32_t k, int32_t n) {
    if (k < 0 || k >= 6 || n < 0 || n >= 64) {
        return 0;
    }
    return Binomial[k][n];
}

static inline int32_t lead_pawns_size_at(int32_t lead_cnt, size_t f) {
    if (lead_cnt < 0 || lead_cnt >= 6 || f >= 4) {
        return 0;
    }
    return LeadPawnsSize[lead_cnt][f];
}

static inline int32_t lead_pawn_idx_at(size_t lead_cnt, unsigned sq) {
    if (lead_cnt >= 6 || sq >= 64) {
        return 0;
    }
    return LeadPawnIdx[lead_cnt][sq];
}

#endif  // MCFISH_SYZYGY_ENCODE_H
