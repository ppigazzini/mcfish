#include "attacks.h"

#include "bitboard.h"

#include <stddef.h>

#if defined(__BMI2__) || defined(__AVX2__)
    #include <immintrin.h>
#endif

Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard PawnAttacksBB[COLOR_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];

// Size the flat tables to the exact totals the mask popcounts imply. These are
// upstream's constants, not guesses: a rook needs 2^12 entries on the corner
// files and fewer elsewhere, summing to 0x19000; bishops sum to 0x1480. Under-size
// either and init_magics writes past the end of the previous square's block.
static Bitboard RookTable[0x19000];
static Bitboard BishopTable[0x1480];

// Index [square][0] for bishops, [square][1] for rooks.
//
// Align to the cache line, as upstream does (`attacks.cpp:36`). sizeof(Magic) is 32,
// so a square's bishop/rook pair is exactly one line and every site that probes both
// from the same square touches one line instead of two -- but only if the array
// starts on a line. Unaligned it landed at offset 16, which splits the second entry's
// `magic`/`shift` across the boundary on the non-BMI2 path that reads them. The
// static_assert is what keeps the two facts joined: widen Magic and the pair no
// longer fits, silently.
static alignas(64) Magic Magics[SQUARE_NB][2];
static_assert(2 * sizeof(Magic) == 64, "a square's bishop/rook Magic pair must be one cache line");

#ifdef __AVX2__
// Upstream's dual hyperbola quintessence (attacks.h:91, attacks.cpp:83). The four
// masks MUST be the first 32 bytes and the struct 32-aligned: both_attacks_bb loads
// them as a single __m256i.
typedef struct {
    // The four masks must be FIRST: both_attacks_bb loads them as one __m256i, and
    // the array below is 64-aligned so every element is 32-aligned as that load needs.
    //
    // The third lane is the RANK, and it is zero unless __GFNI__ solves it: without the
    // intra-byte bit reversal below, a rank's eight squares all share one byte and the
    // byte reversal moves none of them past each other, so hyperbola quintessence on
    // that lane returns nothing and the scalar lookup answers instead.
    Bitboard mask_file, mask_diag, mask_rank, mask_antidiag;
    // 2 * square_bb(s), 2 * square_bb(63 - s). `rr` is the FULL bit reversal's image of
    // `r`, which is what the GFNI lane performs; the byte-reversing form agrees with it
    // only because no mask can reach the bits the two disagree on.
    Bitboard r, rr;
    // The scalar rank reader's two fields. They stay in the struct at every tier: the
    // stride of the array below is what makes each element's mask quad 32-aligned for
    // the load above, and the static_assert is what holds that.
    const uint8_t *rank_attacks_lookup;
    int shift;  // 8 * rank_of(s)
} DualMagic;

static alignas(64) DualMagic DualMagics[SQUARE_NB];
static_assert(sizeof(DualMagic) == 64, "DualMagic must stay one cache line");

// Sliding attacks within a rank, indexed by the slider's FILE and the SIX INNER bits
// of the rank occupancy, giving the 8-bit attack set on that rank. Rank attacks are
// the one direction hyperbola quintessence cannot do with a byte reversal, because all
// eight squares share a byte (upstream attacks.cpp:69). A blocker on either edge
// square of the rank cannot change what is attacked beyond it, so those two bits carry
// no information and dropping them divides the table by four.
static alignas(64) uint8_t RankAttacks[FILE_NB][64];
#endif

static constexpr Direction RookDirs[4] = { NORTH, EAST, SOUTH, WEST };
static constexpr Direction BishopDirs[4] = { NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST };

static int magic_slot(PieceType pt) { return pt == ROOK ? 1 : 0; }

// Step one square in D from S, returning SQ_NONE when the step leaves the board.
// Guard on the FILE distance, not just the index range: NORTH_EAST from H4 lands
// on A5, which is on the board and wrong.
static Square safe_step(Square s, Direction d) {
    const int to = (int) s + (int) d;
    if (to < 0 || to >= SQUARE_NB)
        return SQ_NONE;

    const int df = file_of((Square) to) - file_of(s);
    if (df < -2 || df > 2)
        return SQ_NONE;

    return (Square) to;
}

// Ray-cast PT's attack set from S, stopping on and including the first blocker.
// This is the reference the magic search is validated against, and it stays in
// the binary as the source of truth for building the tables.
static Bitboard sliding_attack(PieceType pt, Square s, Bitboard occupied) {
    Bitboard attacks = 0;
    const Direction *dirs = (pt == ROOK) ? RookDirs : BishopDirs;

    for (int i = 0; i < 4; ++i) {
        Square sq = s;
        while ((sq = safe_step(sq, dirs[i])) != SQ_NONE) {
            attacks |= square_bb(sq);
            if (occupied & square_bb(sq))
                break;
        }
    }
    return attacks;
}

// xorshift64* — the generator and the seeds are fixed, so the magic search
// explores candidates in one reproducible order and produces one fixed set of
// tables. Do not "improve" this: a different generator yields different magics,
// which is harmless for correctness but makes a table dump incomparable.
typedef struct {
    uint64_t state;
} Prng;

static uint64_t prng_rand64(Prng *p) {
    p->state ^= p->state >> 12;
    p->state ^= p->state << 25;
    p->state ^= p->state >> 27;
    return p->state * 2685821657736338717ULL;
}

// AND three draws together: magics need few set bits, and a sparse candidate is
// far likelier to be collision-free than a uniform one.
static uint64_t prng_sparse_rand(Prng *p) {
    return prng_rand64(p) & prng_rand64(p) & prng_rand64(p);
}

static constexpr uint64_t MagicSeeds[8] = { 728, 10316, 55013, 32803, 12281, 15100, 16645, 255 };

// Index a square's attack block. Mirror upstream's USE_PEXT split (upstream
// `bitboard.h: Magic::index`): with BMI2 available, `pext` compresses the
// masked occupancy into a dense index in one instruction, replacing the
// and/multiply/shift of the magic path on every sliding-attack lookup. The two
// paths yield different table layouts but each fills and probes with the same
// function, so the attack sets — and the search — are identical. Under PEXT the
// index is injective on mask subsets, so the magic search below accepts its
// first candidate and `magic`/`shift` go unused.
static unsigned magic_index(const Magic *m, Bitboard occupied) {
#ifdef __BMI2__
    return (unsigned) _pext_u64(occupied, m->mask);
#else
    return (unsigned) (((occupied & m->mask) * m->magic) >> m->shift);
#endif
}

#ifdef __AVX2__
// The ray through S along D1/D2, excluding S. Upstream's `line_mask`
// (attacks.cpp:81).
static Bitboard line_mask(Square sq, Direction d1, Direction d2) {
    Bitboard mask = 0;
    const Direction dirs[2] = { d1, d2 };
    for (int i = 0; i < 2; ++i) {
        Square s = sq;
        while ((s = safe_step(s, dirs[i])) != SQ_NONE)
            mask |= square_bb(s);
    }
    return mask;
}

static void init_dual_magics(void) {
    for (int file = 0; file < FILE_NB; ++file)
        for (int occ6 = 0; occ6 < 64; ++occ6)
            RankAttacks[file][occ6] =
              (uint8_t) sliding_attack(ROOK, (Square) file, (Bitboard) (unsigned) (occ6 << 1));

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        DualMagic *const m = &DualMagics[s];
        m->mask_file = line_mask(s, NORTH, SOUTH);
        m->mask_diag = line_mask(s, NORTH_EAST, SOUTH_WEST);
    #ifdef __GFNI__
        m->mask_rank = line_mask(s, EAST, WEST);
    #else
        m->mask_rank = 0;
    #endif
        m->mask_antidiag = line_mask(s, NORTH_WEST, SOUTH_EAST);
        m->r = square_bb(s) * 2;
        m->rr = square_bb((Square) (63 - (int) s)) * 2;
        m->rank_attacks_lookup = RankAttacks[file_of(s)];
        m->shift = 8 * (int) rank_of(s);
    }
}
#endif

// Search a collision-free magic per square and fill its attack block.
//
// The `epoch` trick avoids clearing the table between candidates: a slot is stale
// unless it was stamped with the current attempt number, so a failed candidate
// costs nothing to abandon.
static void init_magics(PieceType pt, Bitboard *table) {
    Bitboard occupancy[4096];
    Bitboard reference[4096];
    int epoch[4096] = { 0 };
    int cnt = 0;
    size_t previous_size = 0;
    const int slot = magic_slot(pt);

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        // Exclude the board edges from the mask: a blocker on the far edge cannot
        // change what is attacked beyond it, so it carries no information and
        // dropping it halves the table.
        const Bitboard edges = ((rank_bb(0) | rank_bb(7)) & ~rank_bb(rank_of(s)))
                             | ((file_bb(0) | file_bb(7)) & ~file_bb(file_of(s)));

        Magic *m = &Magics[s][slot];
        m->mask = sliding_attack(pt, s, 0) & ~edges;
        m->shift = (unsigned) (64 - popcount_bb(m->mask));

        // Lay each square's block immediately after the previous one.
        m->attacks = (s == SQ_A1) ? table : Magics[s - 1][slot].attacks + previous_size;

        // Enumerate every subset of the mask by the carry-rippling trick.
        size_t size = 0;
        Bitboard subset = 0;
        do {
            occupancy[size] = subset;
            reference[size] = sliding_attack(pt, s, subset);
            ++size;
            subset = (subset - m->mask) & m->mask;
        } while (subset);

        Prng prng = { .state = MagicSeeds[rank_of(s)] };

        for (size_t i = 0; i < size;) {
            // Require the top byte of magic*mask to carry at least 6 bits;
            // candidates below that essentially never spread the index well.
            for (m->magic = 0; popcount_bb((m->magic * m->mask) >> 56) < 6;)
                m->magic = prng_sparse_rand(&prng);

            ++cnt;
            for (i = 0; i < size; ++i) {
                const unsigned idx = magic_index(m, occupancy[i]);

                if (epoch[idx] < cnt) {
                    epoch[idx] = cnt;
                    m->attacks[idx] = reference[i];
                } else if (m->attacks[idx] != reference[i]) {
                    break;  // constructive collision: this magic is unusable
                }
            }
        }

        previous_size = size;
    }
}

#ifdef __AVX2__
// Reverse each 64-bit lane. vpshufb reverses the BYTES, which is all hyperbola
// quintessence needs for a file, a diagonal or an antidiagonal, because every square of
// those rays sits in a distinct byte. Under __GFNI__ one vgf2p8affineqb by the
// anti-diagonal matrix 0x8040201008040201 then reverses the bits INSIDE every byte, and
// the pair is a full 64-bit reversal -- which is the one thing a rank needs to join
// them, and it is what the AArch64 path has always done.
//
// Byte order across the two halves of a 128-bit lane is immaterial: `rr` is broadcast
// and the second call undoes the first.
static inline __m256i reverse_lanes(__m256i v, __m256i bswap_ctl) {
    v = _mm256_shuffle_epi8(v, bswap_ctl);
    #ifdef __GFNI__
    v = _mm256_gf2p8affine_epi64_epi8(v, _mm256_set1_epi64x((long long) 0x8040201008040201LL), 0);
    #endif
    return v;
}
#endif

DualAttacks both_attacks_bb(Square s, Bitboard occupied) {
#ifdef __AVX2__
    // Upstream attacks.h:108. One 256-bit load covers file/diag/rank/antidiag; the
    // subtract-reverse-subtract-xor is hyperbola quintessence run on every ray at once.
    // A byte reversal is enough for the three rays whose squares sit in distinct bytes,
    // and under __GFNI__ reverse_lanes makes it a full bit reversal, which is what lets
    // the RANK take the fourth lane instead of the 64-entry lookup below.
    const DualMagic *const m = &DualMagics[s];
    const __m256i bswap_ctl = _mm256_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                              0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const __m256i mask = _mm256_load_si256((const __m256i *) (const void *) m);
    const __m256i rs = _mm256_set1_epi64x((long long) m->r);
    const __m256i rrs = _mm256_set1_epi64x((long long) m->rr);

    const __m256i o = _mm256_and_si256(mask, _mm256_set1_epi64x((long long) occupied));
    const __m256i fwd = _mm256_sub_epi64(o, rs);
    const __m256i rev =
      reverse_lanes(_mm256_sub_epi64(reverse_lanes(o, bswap_ctl), rrs), bswap_ctl);
    const __m256i result = _mm256_and_si256(_mm256_xor_si256(fwd, rev), mask);

    // Lanes 0 and 2 carry the rook rays, lanes 1 and 3 the two diagonals; the OR across
    // the 128-bit halves folds each pair into one.
    const __m128i rook_bishop =
      _mm_or_si128(_mm256_extracti128_si256(result, 1), _mm256_castsi256_si128(result));

    #ifdef __GFNI__
    // Lane 2 already carried the rank, so the rook attacks are complete.
    return (DualAttacks) {
        .bishop = (Bitboard) _mm_extract_epi64(rook_bishop, 1),
        .rook = (Bitboard) _mm_cvtsi128_si64(rook_bishop),
    };
    #else
    const Bitboard rank_attacks =
      (Bitboard) m->rank_attacks_lookup[(occupied >> (m->shift + 1)) & 0x3f] << m->shift;

    return (DualAttacks) {
        .bishop = (Bitboard) _mm_extract_epi64(rook_bishop, 1),
        .rook = (Bitboard) _mm_cvtsi128_si64(rook_bishop) + rank_attacks,
    };
    #endif
#else
    // Two magic lookups, which is what upstream's non-dual path does.
    return (DualAttacks) { .bishop = attacks_bb(BISHOP, s, occupied),
                           .rook = attacks_bb(ROOK, s, occupied) };
#endif
}

Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    switch (pt) {
    case BISHOP :
    case ROOK : {
        const Magic *m = &Magics[s][magic_slot(pt)];
        return m->attacks[magic_index(m, occupied)];
    }
    case QUEEN :
        return attacks_bb(BISHOP, s, occupied) | attacks_bb(ROOK, s, occupied);
    default :
        return PseudoAttacks[pt][s];
    }
}

void attacks_init(void) {
#ifdef __AVX2__
    init_dual_magics();
#endif
    static constexpr Direction KnightSteps[8] = { 17, 15, 10, 6, -6, -10, -15, -17 };
    static constexpr Direction KingSteps[8] = { 9, 8, 7, 1, -1, -7, -8, -9 };

    init_magics(ROOK, RookTable);
    init_magics(BISHOP, BishopTable);

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        PawnAttacksBB[WHITE][s] = pawn_attacks_bb(WHITE, square_bb(s));
        PawnAttacksBB[BLACK][s] = pawn_attacks_bb(BLACK, square_bb(s));

        for (int i = 0; i < 8; ++i) {
            const Square k = safe_step(s, KnightSteps[i]);
            if (k != SQ_NONE)
                PseudoAttacks[KNIGHT][s] |= square_bb(k);

            const Square g = safe_step(s, KingSteps[i]);
            if (g != SQ_NONE)
                PseudoAttacks[KING][s] |= square_bb(g);
        }

        // Derive the empty-board slider reach THROUGH the magics rather than from
        // sliding_attack: it exercises the freshly built tables on every square at
        // startup, so a broken magic surfaces here instead of mid-search.
        PseudoAttacks[BISHOP][s] = attacks_bb(BISHOP, s, 0);
        PseudoAttacks[ROOK][s] = attacks_bb(ROOK, s, 0);
        PseudoAttacks[QUEEN][s] = PseudoAttacks[BISHOP][s] | PseudoAttacks[ROOK][s];
    }

    // BetweenBB includes s2 so an evasion mask covers both blocking a slider and
    // capturing it; LineBB is the full line through both, or 0 when not aligned.
    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
        for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
            BetweenBB[s1][s2] = square_bb(s2);

            for (PieceType pt = BISHOP; pt <= ROOK; ++pt)
                if (PseudoAttacks[pt][s1] & square_bb(s2)) {
                    LineBB[s1][s2] = (attacks_bb(pt, s1, 0) & attacks_bb(pt, s2, 0)) | square_bb(s1)
                                   | square_bb(s2);
                    BetweenBB[s1][s2] =
                      (attacks_bb(pt, s1, square_bb(s2)) & attacks_bb(pt, s2, square_bb(s1)))
                      | square_bb(s2);
                }
        }
}
