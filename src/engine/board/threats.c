#include "threats.h"

#include <string.h>

#include "attacks.h"
#include "bitboard.h"

// Hold the ray-pass geometry, built once from the attack tables at startup and
// read-only during search. Without it every slider in every threat update
// re-ray-casts, and the threat update runs per piece touched per node.
static Bitboard RayPassBB[SQUARE_NB][SQUARE_NB];

void threats_init(void) {
    memset(RayPassBB, 0, sizeof RayPassBB);

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

#ifdef MCFISH_DIRTY_THREAT_VECTOR
// Write every threat in MASK in one pass, as upstream's write_multiple_dirties does
// (position.cpp:1157).
//
// The scalar loop this replaces is up to 16 iterations of pop_lsb -> board[sq] ->
// pack -> append, each dependent on the last. Here the whole 64-square board is one
// register (it is 64 bytes), `compress_epi8` turns the bitboard into the packed list
// of squares in one instruction, `permutexvar_epi8` looks up all 16 pieces from the
// board register with no memory access at all, and one ternarylogic ORs the constant
// template together with both shifted fields.
//
// TEMPLATE carries the fields that do not vary across the mask; SQ_SHIFT and PC_SHIFT
// place the two that do.
//
// The store is 64 bytes and UNMASKED, as upstream's is: DIRTY_THREAT_MAX is 96 with a
// documented 16-slot tail precisely so this cannot run off the end (types.h).
static inline void write_multiple_dirties(const Position *pos,
                                          Bitboard mask,
                                          uint32_t template_word,
                                          int sq_shift,
                                          int pc_shift,
                                          DirtyThreats *dts) {
    static_assert(sizeof(uint32_t) == 4, "a packed DirtyThreat is one 32-bit lane");

    const int count = popcount_bb(mask);
    if (count == 0)
        return;

    // Bytes 0..63, so compressing under the bitboard yields the square list.
    const __m512i all_squares = _mm512_set_epi8(
      63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41,
      40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18,
      17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);

    const __m512i board = _mm512_loadu_si512((const void *) pos->board);
    const __m512i template_v = _mm512_set1_epi32((int) template_word);

    __m512i squares = _mm512_maskz_compress_epi8((__mmask64) mask, all_squares);
    squares = _mm512_cvtepi8_epi32(_mm512_castsi512_si128(squares));

    // Index the board by those squares; the mask keeps the low byte of each lane.
    __m512i pieces_v = _mm512_maskz_permutexvar_epi8(0x1111111111111111ULL, squares, board);

    squares = _mm512_slli_epi32(squares, sq_shift);
    pieces_v = _mm512_slli_epi32(pieces_v, pc_shift);

    // 254 is A | B | C.
    const __m512i dirties = _mm512_ternarylogic_epi32(template_v, squares, pieces_v, 254);
    _mm512_storeu_si512((void *) &dts->list_values[dts->list_size], dirties);
    dts->list_size += (size_t) count;
}
#endif

static inline void add_dirty_threat(
  DirtyThreats *dts, bool put_piece, Piece pc, Piece threatened, Square s, Square threatened_sq) {
    dts->list_values[dts->list_size] =
      dirty_threat_make(put_piece, pc, threatened, s, threatened_sq);
    dts->list_size++;
}

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

// Compile the body once per compute_ray value, as upstream's template
// instantiation does (position.cpp:1183 update_piece_threats<ComputeRay>). A
// merged runtime-bool body keeps both variants' state live across the whole
// function — the no-ray-only direct_sliders value survives the ray path and the
// register allocator spills for the union of both paths — where a specialized
// copy lets the ray variant drop it entirely.
__attribute__((always_inline)) static inline void threats_update_piece_impl(bool compute_ray,
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
    // Both ray sets in one pass, as upstream's update_piece_threats does
    // (position.cpp:1203, `both_attacks_bb(s, occupied)`).
    const DualAttacks slider_attacks = both_attacks_bb(s, occupied);
    const Bitboard r_attacks = slider_attacks.rook;
    const Bitboard b_attacks = slider_attacks.bishop;
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

    // Reuse the prologue's magic lookups for the slider types instead of
    // re-deriving attacks_bb(pt, s, occupied): r_attacks/b_attacks ARE those
    // values. Upstream's compiled update_piece_threats does the same reuse (clang
    // CSEs the template's attacks_bb switch against the prologue); spelling it out
    // keeps the C build from re-running a masked magic multiply per slider call.
    Bitboard threatened = (pt == PAWN     ? PawnAttacksBB[color_of_piece(pc)][s]
                           : pt == BISHOP ? b_attacks
                           : pt == ROOK   ? r_attacks
                           : pt == QUEEN  ? (r_attacks | b_attacks)
                                          : PseudoAttacks[pt][s])
                        & occupied_no_k;
    Bitboard incoming = PseudoAttacks[KNIGHT][s] & knights;

    // Restrict both directions to the (attacker, attacked) pairs the threat feature
    // set actually encodes — upstream rejects the rest here rather than letting the
    // feature indexer drop them later. With SFNNv16 the pawn-pawn relationships moved
    // to the PP_3Wide feature set, so a pawn is no longer a threat target and only a
    // knight or a rook records an incoming pawn threat. The pusher block is gone.
    if (pt == KNIGHT || pt == ROOK)
        incoming |=
          (PawnAttacksBB[WHITE][s] & black_pawns) | (PawnAttacksBB[BLACK][s] & white_pawns);

    switch (pt) {
    case PAWN :
        threatened &= pos->by_type[KNIGHT] | pos->by_type[ROOK];
        break;
    case BISHOP :
    case ROOK :
        threatened &=
          pos->by_type[PAWN] | pos->by_type[KNIGHT] | pos->by_type[BISHOP] | pos->by_type[ROOK];
        break;
    default :
        break;  // already masked by occupied_no_k
    }

#ifdef MCFISH_DIRTY_THREAT_VECTOR
    // Outgoing: PC on S threatens the piece on each square of `threatened`. The
    // template fixes `add`, `pc` and `pc_sq`; the square and the threatened piece
    // vary (upstream position.cpp:1269).
    write_multiple_dirties(
      pos, threatened, dirty_threat_make(put_piece, pc, NO_PIECE, s, (Square) 0),
      DIRTY_THREAT_THREATENED_SQ_OFFSET, DIRTY_THREAT_THREATENED_PC_OFFSET, dts);

    // Incoming: the piece on each square threatens PC on S. Upstream folds the
    // direct sliders in HERE and then tells process_sliders not to emit them again,
    // which is the control-flow half of this port -- the scalar path instead folds
    // them in only on the !compute_ray branch (upstream position.cpp:1273, 1293).
    const Bitboard all_attackers = direct_sliders | incoming;
    write_multiple_dirties(pos, all_attackers,
                           dirty_threat_make(put_piece, NO_PIECE, pc, (Square) 0, s),
                           DIRTY_THREAT_PC_SQ_OFFSET, DIRTY_THREAT_PC_OFFSET, dts);

    if (compute_ray)
        process_sliders(pos, dts, sliders, s, pc, put_piece, no_rays, r_attacks, b_attacks,
                        occupied_no_k, false);
#else
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
#endif
}

// The two instantiations upstream's template emits. Left inlinable: see threats.h.
void threats_update_piece_ray(
  const Position *pos, Piece pc, bool put_piece, Square s, DirtyThreats *dts, Bitboard no_rays) {
    threats_update_piece_impl(true, pos, pc, put_piece, s, dts, no_rays);
}

void threats_update_piece_no_ray(
  const Position *pos, Piece pc, bool put_piece, Square s, DirtyThreats *dts, Bitboard no_rays) {
    threats_update_piece_impl(false, pos, pc, put_piece, s, dts, no_rays);
}
