#include "attacks.h"

#include "bitboard.h"

#include <stddef.h>

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

// Index [square][0] for bishops, [square][1] for rooks — the same layout zfish
// uses via magicIndexForPiece.
static Magic Magics[SQUARE_NB][2];

static const Direction RookDirs[4] = { NORTH, EAST, SOUTH, WEST };
static const Direction BishopDirs[4] = { NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST };

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

// xorshift64* — the same generator and the same seeds zfish uses, so the magic
// search explores candidates in the identical order and produces the identical
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

static const uint64_t MagicSeeds[8] = { 728, 10316, 55013, 32803, 12281, 15100, 16645, 255 };

static unsigned magic_index(const Magic *m, Bitboard occupied) {
    return (unsigned) (((occupied & m->mask) * m->magic) >> m->shift);
}

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
    static const Direction KnightSteps[8] = { 17, 15, 10, 6, -6, -10, -15, -17 };
    static const Direction KingSteps[8] = { 9, 8, 7, 1, -1, -7, -8, -9 };

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
