#include "threats.h"

#include <string.h>

#include "attacks.h"
#include "bitboard.h"

// Hold the ray-pass geometry, built once from the attack tables at startup and
// read-only during search. Without it every slider in every threat update
// re-ray-casts, and the threat update runs per piece touched per node.
static Bitboard RayPassBB[SQUARE_NB][SQUARE_NB];

// The single push square plus the two attack squares of a pawn of each color on
// each square, precomputed so the per-node threat update reads one table entry
// instead of a shift + OR + attack-table load (upstream PawnPushOrAttacks,
// attacks.h:250).
static Bitboard PawnPushOrAttacks[COLOR_NB][SQUARE_NB];

void threats_init(void) {
    memset(RayPassBB, 0, sizeof RayPassBB);

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        const Bitboard b = square_bb(s);
        PawnPushOrAttacks[WHITE][s] = (b << 8) | PawnAttacksBB[WHITE][s];
        PawnPushOrAttacks[BLACK][s] = (b >> 8) | PawnAttacksBB[BLACK][s];
    }

    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1) {
        const PieceType slider_types[2] = { BISHOP, ROOK };
        for (int t = 0; t < 2; ++t) {
            const PieceType pt = slider_types[t];
            const Bitboard from_empty = attacks_bb(pt, s1, 0);
            for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
                if ((from_empty & square_bb(s2)) == 0)
                    continue;
                RayPassBB[s1][s2] =
                  from_empty & (attacks_bb(pt, s2, square_bb(s1)) | square_bb(s2));
            }
        }
    }
}

Bitboard ray_pass_bb(Square s1, Square s2) { return RayPassBB[s1][s2]; }

static inline void add_dirty_threat(
  DirtyThreats *dts, bool put_piece, Piece pc, Piece threatened, Square s, Square threatened_sq) {
    dts->list_values[dts->list_size] =
      dirty_threat_make(put_piece, pc, threatened, s, threatened_sq);
    dts->list_size++;
}

// Return the single push square plus the two attack squares of a color-C pawn on
// S. The push cannot wrap a file, so it needs no edge mask.
static inline Bitboard pawn_push_or_attacks(Color c, Square s) { return PawnPushOrAttacks[c][s]; }

// Count a threatened queen as a threat feature only when the slider is itself a
// queen; every other threatened type always counts. Mirrors upstream
// can_slider_threat (position.cpp:1178). Rejecting here is what keeps the dirty
// list to the set the feature indexer accepts — the combinations filtered out are
// exactly those the index mapping sends out of range and the accumulator then
// discards, so recording them was pure work.
static inline bool can_slider_threat(Piece pc, Piece slider) {
    return type_of_piece(pc) != QUEEN || type_of_piece(slider) == QUEEN;
}

static void process_sliders(const Position *pos,
                            DirtyThreats *dts,
                            Bitboard sliders_in,
                            Square s,
                            Piece pc,
                            bool put_piece,
                            Bitboard no_rays,
                            Bitboard r_attacks,
                            Bitboard b_attacks,
                            Bitboard occupied_no_k,
                            bool add_direct) {
    Bitboard sliders = sliders_in;
    while (sliders != 0) {
        const Square slider_sq = pop_lsb(&sliders);
        const Piece slider = pos->board[slider_sq];

        const Bitboard ray = RayPassBB[slider_sq][s];
        const Bitboard discovered = ray & (r_attacks | b_attacks) & occupied_no_k;

        if (discovered != 0 && (ray & no_rays) != no_rays) {
            const Square tsq = lsb(discovered);
            const Piece tpc = pos->board[tsq];
            if (can_slider_threat(tpc, slider))
                add_dirty_threat(dts, !put_piece, slider, tpc, slider_sq, tsq);
        }

        if (add_direct && can_slider_threat(pc, slider))
            add_dirty_threat(dts, put_piece, slider, pc, slider_sq, s);
    }
}

void threats_update_piece(bool compute_ray,
                          const Position *pos,
                          Piece pc,
                          bool put_piece,
                          Square s,
                          DirtyThreats *dts,
                          Bitboard no_rays) {
    const PieceType pt = type_of_piece(pc);
    const Bitboard occupied = pos->by_type[ALL_PIECES];
    const Bitboard rook_queens = pos->by_type[ROOK] | pos->by_type[QUEEN];
    const Bitboard bishop_queens = pos->by_type[BISHOP] | pos->by_type[QUEEN];
    const Bitboard r_attacks = attacks_bb(ROOK, s, occupied);
    const Bitboard b_attacks = attacks_bb(BISHOP, s, occupied);
    const Bitboard occupied_no_k = occupied ^ pos->by_type[KING];

    const Bitboard sliders = (rook_queens & r_attacks) | (bishop_queens & b_attacks);
    // Apply can_slider_threat in bitboard form: a threatened queen only counts
    // against a queen.
    const Bitboard direct_sliders = (pt == QUEEN) ? sliders & pos->by_type[QUEEN] : sliders;

    if (pt == KING) {
        if (compute_ray)
            process_sliders(pos, dts, sliders, s, pc, put_piece, no_rays, r_attacks, b_attacks,
                            occupied_no_k, false);
        return;
    }

    const Bitboard knights = pos->by_type[KNIGHT];
    const Bitboard white_pawns = pos->by_color[WHITE] & pos->by_type[PAWN];
    const Bitboard black_pawns = pos->by_color[BLACK] & pos->by_type[PAWN];

    Bitboard threatened =
      (pt == PAWN ? PawnAttacksBB[color_of_piece(pc)][s] : attacks_bb(pt, s, occupied))
      & occupied_no_k;
    Bitboard incoming = PseudoAttacks[KNIGHT][s] & knights;

    // Compute both incoming and outgoing pawn threats. Incoming pawn pushers count
    // only when `pc` is itself a pawn.
    Bitboard pawn_threats = 0;
    if (pt == PAWN) {
        const Bitboard white_attacks = pawn_push_or_attacks(WHITE, s);
        const Bitboard black_attacks = pawn_push_or_attacks(BLACK, s);

        threatened |=
          (color_of_piece(pc) == WHITE ? white_attacks : black_attacks) & pos->by_type[PAWN];

        pawn_threats = (white_attacks & black_pawns) | (black_attacks & white_pawns);
    } else {
        pawn_threats =
          (PawnAttacksBB[WHITE][s] & black_pawns) | (PawnAttacksBB[BLACK][s] & white_pawns);
    }

    // Restrict both directions to the (attacker, attacked) pairs the threat feature
    // set actually encodes — upstream rejects the rest here rather than letting the
    // feature indexer drop them later.
    if (pt == PAWN || pt == KNIGHT || pt == ROOK)
        incoming |= pawn_threats;

    switch (pt) {
    case PAWN :
        threatened &= pos->by_type[PAWN] | pos->by_type[KNIGHT] | pos->by_type[ROOK];
        break;
    case BISHOP :
    case ROOK :
        threatened &=
          pos->by_type[PAWN] | pos->by_type[KNIGHT] | pos->by_type[BISHOP] | pos->by_type[ROOK];
        break;
    default :
        break;  // already masked by occupied_no_k
    }

    while (threatened != 0) {
        const Square tsq = pop_lsb(&threatened);
        add_dirty_threat(dts, put_piece, pc, pos->board[tsq], s, tsq);
    }

    if (compute_ray) {
        process_sliders(pos, dts, sliders, s, pc, put_piece, no_rays, r_attacks, b_attacks,
                        occupied_no_k, true);
    } else {
        incoming |= direct_sliders;
    }

    while (incoming != 0) {
        const Square src_sq = pop_lsb(&incoming);
        add_dirty_threat(dts, put_piece, pos->board[src_sq], pc, src_sq, s);
    }
}
