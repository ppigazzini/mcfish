#include "movepick.h"

#include "history.h"

#include "../board/attacks.h"
#include "../board/bitboard.h"
#include "../board/movegen.h"
#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>
#include <stdint.h>

// Index the piece values by PIECE, not by PieceType: the capture and evasion
// scorers read `PieceValue[pos.piece_on(to))]` straight off the board, so the
// table repeats for the black half and leaves the two encoding gaps at 0.
static const int PieceValues[PIECE_NB] = {
    0, 208, 781, 825, 1276, 2538, 0, 0, 0, 208, 781, 825, 1276, 2538, 0, 0,
};

enum { KIND_CAPTURES = 0, KIND_QUIETS = 1, KIND_EVASIONS = 2 };

enum { GOOD_QUIET_THRESHOLD = -14000 };

static inline Bitboard least_significant_bb(Bitboard b) { return b & (~b + 1); }

bool see_ge(const Position *pos, Move m, int threshold) {
    if (move_type(m) != NORMAL)
        return 0 >= threshold;

    const Square from = move_from(m);
    const Square to = move_to(m);

    int swap = piece_value(type_of_piece(piece_on(pos, to))) - threshold;
    if (swap < 0)
        return false;

    swap = piece_value(type_of_piece(piece_on(pos, from))) - swap;
    if (swap <= 0)
        return true;

    Bitboard occupied = pieces(pos) ^ square_bb(from) ^ square_bb(to);
    Color stm = pos->side_to_move;
    Bitboard attackers = pos_attackers_to_occ(pos, to, occupied);
    int res = 1;

    const Bitboard bishops_queens = pos->by_type[BISHOP] | pos->by_type[QUEEN];
    const Bitboard rooks_queens = pos->by_type[ROOK] | pos->by_type[QUEEN];

    for (;;) {
        stm = flip_color(stm);
        attackers &= occupied;

        Bitboard stm_attackers = attackers & pieces_c(pos, stm);
        if (stm_attackers == 0)
            break;

        // Ignore an attacker that is pinned to its own king, but only while the
        // pinner is still on the board.
        if ((pos->st->pinners[flip_color(stm)] & occupied) != 0) {
            stm_attackers &= ~pos->st->blockers[stm];
            if (stm_attackers == 0)
                break;
        }

        res ^= 1;

        Bitboard bb;
        if ((bb = stm_attackers & pos->by_type[PAWN]) != 0) {
            swap = PAWN_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
            attackers |= attacks_bb(BISHOP, to, occupied) & bishops_queens;
        } else if ((bb = stm_attackers & pos->by_type[KNIGHT]) != 0) {
            swap = KNIGHT_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
        } else if ((bb = stm_attackers & pos->by_type[BISHOP]) != 0) {
            swap = BISHOP_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
            attackers |= attacks_bb(BISHOP, to, occupied) & bishops_queens;
        } else if ((bb = stm_attackers & pos->by_type[ROOK]) != 0) {
            swap = ROOK_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
            attackers |= attacks_bb(ROOK, to, occupied) & rooks_queens;
        } else if ((bb = stm_attackers & pos->by_type[QUEEN]) != 0) {
            swap = QUEEN_VALUE - swap;
            occupied ^= least_significant_bb(bb);
            attackers |= (attacks_bb(BISHOP, to, occupied) & bishops_queens)
                       | (attacks_bb(ROOK, to, occupied) & rooks_queens);
        } else {
            // Only the king is left: reverse the result if the opponent still has
            // an attacker, because capturing into check is not available.
            return (attackers & ~pieces_c(pos, stm)) != 0 ? (res ^ 1) != 0 : res != 0;
        }
    }

    return res != 0;
}

// Return the squares from which PT gives check to the side not to move.
static Bitboard check_squares(const Position *pos, PieceType pt) {
    const Color them = flip_color(pos->side_to_move);
    const Square ksq = king_square(pos, them);
    const Bitboard occ = pieces(pos);

    switch (pt) {
    case PAWN :
        return pawn_attacks_bb(them, square_bb(ksq));
    case KNIGHT :
        return attacks_bb(KNIGHT, ksq, occ);
    case BISHOP :
        return attacks_bb(BISHOP, ksq, occ);
    case ROOK :
        return attacks_bb(ROOK, ksq, occ);
    case QUEEN :
        return attacks_bb(BISHOP, ksq, occ) | attacks_bb(ROOK, ksq, occ);
    default :
        return 0;
    }
}

// Return the union of the attacks of C's PT pieces. Pawns resolve as a set in two
// shifts, as upstream's attacks_by<PAWN> does; the other types walk piece by piece.
static Bitboard attacks_by(const Position *pos, Color c, PieceType pt) {
    if (pt == PAWN)
        return pawn_attacks_bb(c, pieces_cp(pos, c, PAWN));

    Bitboard b = pieces_cp(pos, c, pt);
    Bitboard result = 0;
    while (b != 0)
        result |= attacks_bb(pt, pop_lsb(&b), pieces(pos));

    return result;
}

static inline bool capture_stage(const Position *pos, Move m) {
    return is_capture(pos, m) || move_promotion(m) == QUEEN;
}

static inline int cont_hist_score(const MovePicker *mp, size_t slot, Piece pc, Square to) {
    return mp->cont_hist[slot][(size_t) pc * SQUARE_NB + (size_t) to];
}

// Generate the KIND move list into OUT and fill each entry's ordering value.
// Return the number of moves written.
static size_t score_list(const MovePicker *mp, int kind, ExtMove *out) {
    const Position *pos = mp->pos;
    Histories *h = mp->hist;

    const GenType gen = kind == KIND_CAPTURES ? GEN_CAPTURES
                      : kind == KIND_QUIETS   ? GEN_QUIETS
                                              : GEN_EVASIONS;
    const size_t count = (size_t) (generate(pos, out, gen) - out);

    const Color us = pos->side_to_move;

    // Collect, per moving piece type, the squares attacked by a strictly cheaper
    // enemy piece. Quiet scoring pays for stepping into one and rewards leaving one.
    Bitboard threat_by_lesser[PIECE_TYPE_NB] = { 0 };
    if (kind == KIND_QUIETS) {
        const Color them = flip_color(us);
        threat_by_lesser[PAWN] = 0;
        threat_by_lesser[KNIGHT] = attacks_by(pos, them, PAWN);
        threat_by_lesser[BISHOP] = threat_by_lesser[KNIGHT];
        threat_by_lesser[ROOK] =
          attacks_by(pos, them, KNIGHT) | attacks_by(pos, them, BISHOP) | threat_by_lesser[KNIGHT];
        threat_by_lesser[QUEEN] = attacks_by(pos, them, ROOK) | threat_by_lesser[ROOK];
        threat_by_lesser[KING] = 0;
    }

    for (size_t i = 0; i < count; ++i) {
        const Move m = out[i].move;
        const Square from = move_from(m);
        const Square to = move_to(m);
        const Piece pc = piece_on(pos, from);
        const PieceType pt = type_of_piece(pc);
        const Piece captured = piece_on(pos, to);

        int value = 0;

        if (kind == KIND_CAPTURES) {
            value = *capture_entry(h, pc, to, type_of_piece(captured)) + 7 * PieceValues[captured];
        } else if (kind == KIND_QUIETS) {
            const int main_history = h->main_history[(size_t) us * HIST_UINT16 + (size_t) m];
            const int pawn_history =
              pawn_history_row(h, mp->pawn_key)[(size_t) pc * SQUARE_NB + (size_t) to];
            const int continuation_sum =
              cont_hist_score(mp, 0, pc, to) + cont_hist_score(mp, 1, pc, to)
              + cont_hist_score(mp, 2, pc, to) + cont_hist_score(mp, 3, pc, to)
              + cont_hist_score(mp, 5, pc, to);

            const int check_bonus =
              (check_squares(pos, pt) & square_bb(to)) != 0 && see_ge(pos, m, -75);
            const int from_threatened = (threat_by_lesser[pt] & square_bb(from)) != 0;
            const int to_threatened = (threat_by_lesser[pt] & square_bb(to)) != 0;

            int low_ply_bonus = 0;
            if (mp->ply < LOW_PLY_HISTORY_SIZE) {
                const int low_ply = h->low_ply_history[(size_t) mp->ply * HIST_UINT16 + (size_t) m];
                low_ply_bonus = 8 * low_ply / (1 + mp->ply);
            }

            value = 2 * main_history + 2 * pawn_history + continuation_sum + check_bonus * 16384
                  + PieceValues[pt] * 20 * (from_threatened - to_threatened) + low_ply_bonus;
        } else {
            if (capture_stage(pos, m)) {
                value = PieceValues[captured] + (1 << 28);
            } else {
                value = h->main_history[(size_t) us * HIST_UINT16 + (size_t) m]
                      + cont_hist_score(mp, 0, pc, to);
            }
        }

        out[i].value = value;
    }

    return count;
}

// Sort the entries whose value is at least LIMIT to the front, in descending
// order, leaving the rest where they are. Entry 0 is the initial sorted head and
// is never tested against LIMIT, exactly as upstream's partial_insertion_sort.
static void partial_insertion_sort(ExtMove *entries, size_t count, int limit) {
    if (count == 0)
        return;

    size_t sorted_end = 0;

    for (size_t scan = 1; scan < count; ++scan) {
        if (entries[scan].value >= limit) {
            const ExtMove current = entries[scan];
            ++sorted_end;
            entries[scan] = entries[sorted_end];

            size_t insert_at = sorted_end;
            while (insert_at != 0 && entries[insert_at - 1].value < current.value) {
                entries[insert_at] = entries[insert_at - 1];
                --insert_at;
            }
            entries[insert_at] = current;
        }
    }
}

static void init_common(MovePicker *mp, const Position *pos, Histories *h, Move tt_move) {
    mp->pos = pos;
    mp->hist = h;
    mp->pawn_key = 0;
    for (size_t i = 0; i < 6; ++i)
        mp->cont_hist[i] = nullptr;
    mp->ply = 0;
    mp->tt_move = tt_move;
    mp->threshold = 0;
    mp->depth = 0;
    mp->skip_quiets = false;
    mp->cur = 0;
    mp->end_cur = 0;
    mp->end_bad_captures = 0;
    mp->end_captures = 0;
    mp->end_generated = 0;
}

void movepick_init(MovePicker *mp,
                   const Position *pos,
                   Histories *h,
                   Key pawn_key,
                   Move tt_move,
                   int depth,
                   int ply,
                   const int16_t *const cont_hist[6]) {
    init_common(mp, pos, h, tt_move);
    mp->pawn_key = pawn_key;
    for (size_t i = 0; i < 6; ++i)
        mp->cont_hist[i] = cont_hist[i];
    mp->ply = ply;
    mp->depth = depth;

    const int base = checkers(pos) != 0 ? MP_EVASION_TT : depth > 0 ? MP_MAIN_TT : MP_QSEARCH_TT;
    mp->stage = base + (tt_move == MOVE_NONE);
}

void movepick_init_probcut(
  MovePicker *mp, const Position *pos, Histories *h, Move tt_move, int threshold) {
    init_common(mp, pos, h, tt_move);
    mp->threshold = threshold;
    mp->stage = MP_PROBCUT_TT + (tt_move == MOVE_NONE);
}

// Return the next entry that is not the TT move, or MOVE_NONE when the current
// span is exhausted.
static Move select_any(MovePicker *mp) {
    while (mp->cur < mp->end_cur) {
        const ExtMove entry = mp->moves[mp->cur++];
        if (entry.move != mp->tt_move)
            return entry.move;
    }
    return MOVE_NONE;
}

// Return the next capture that survives SEE, shuffling the losing ones into the
// [0, end_bad_captures) prefix for the BAD_CAPTURE stage to replay later.
static Move select_good_capture(MovePicker *mp) {
    while (mp->cur < mp->end_cur) {
        const size_t index = mp->cur;
        const ExtMove entry = mp->moves[index];

        if (entry.move != mp->tt_move) {
            if (see_ge(mp->pos, entry.move, -entry.value / 18)) {
                ++mp->cur;
                return entry.move;
            }

            const ExtMove tmp = mp->moves[mp->end_bad_captures];
            mp->moves[mp->end_bad_captures] = mp->moves[index];
            mp->moves[index] = tmp;
            ++mp->end_bad_captures;
        }

        ++mp->cur;
    }
    return MOVE_NONE;
}

static Move select_good_quiet(MovePicker *mp) {
    while (mp->cur < mp->end_cur) {
        const ExtMove entry = mp->moves[mp->cur++];
        if (entry.move != mp->tt_move && entry.value > GOOD_QUIET_THRESHOLD)
            return entry.move;
    }
    return MOVE_NONE;
}

static Move select_bad_quiet(MovePicker *mp) {
    while (mp->cur < mp->end_cur) {
        const ExtMove entry = mp->moves[mp->cur++];
        if (entry.move != mp->tt_move && entry.value <= GOOD_QUIET_THRESHOLD)
            return entry.move;
    }
    return MOVE_NONE;
}

static Move select_probcut(MovePicker *mp) {
    while (mp->cur < mp->end_cur) {
        const ExtMove entry = mp->moves[mp->cur++];
        if (entry.move != mp->tt_move && see_ge(mp->pos, entry.move, mp->threshold))
            return entry.move;
    }
    return MOVE_NONE;
}

Move movepick_next(MovePicker *mp) {
    for (;;) {
        switch (mp->stage) {
        case MP_MAIN_TT :
        case MP_EVASION_TT :
        case MP_QSEARCH_TT :
        case MP_PROBCUT_TT :
            ++mp->stage;
            return mp->tt_move;

        case MP_CAPTURE_INIT :
        case MP_PROBCUT_INIT :
        case MP_QCAPTURE_INIT : {
            mp->cur = 0;
            mp->end_bad_captures = 0;

            const size_t count = score_list(mp, KIND_CAPTURES, mp->moves + mp->cur);

            mp->end_cur = mp->cur + count;
            mp->end_captures = mp->end_cur;
            partial_insertion_sort(mp->moves + mp->cur, count, INT32_MIN);
            ++mp->stage;
            continue;
        }

        case MP_GOOD_CAPTURE : {
            const Move m = select_good_capture(mp);
            if (m != MOVE_NONE)
                return m;

            ++mp->stage;
            continue;
        }

        case MP_QUIET_INIT : {
            if (!mp->skip_quiets) {
                const size_t count = score_list(mp, KIND_QUIETS, mp->moves + mp->cur);

                mp->end_cur = mp->cur + count;
                mp->end_generated = mp->end_cur;
                partial_insertion_sort(mp->moves + mp->cur, count, -3560 * mp->depth);
            }

            ++mp->stage;
            continue;
        }

        case MP_GOOD_QUIET : {
            if (!mp->skip_quiets) {
                const Move m = select_good_quiet(mp);
                if (m != MOVE_NONE)
                    return m;
            }

            mp->cur = 0;
            mp->end_cur = mp->end_bad_captures;
            ++mp->stage;
            continue;
        }

        case MP_BAD_CAPTURE : {
            const Move m = select_any(mp);
            if (m != MOVE_NONE)
                return m;

            mp->cur = mp->end_captures;
            mp->end_cur = mp->end_generated;
            ++mp->stage;
            continue;
        }

        case MP_BAD_QUIET :
            if (!mp->skip_quiets)
                return select_bad_quiet(mp);
            return MOVE_NONE;

        case MP_EVASION_INIT : {
            mp->cur = 0;

            const size_t count = score_list(mp, KIND_EVASIONS, mp->moves + mp->cur);

            mp->end_cur = mp->cur + count;
            mp->end_generated = mp->end_cur;
            partial_insertion_sort(mp->moves + mp->cur, count, INT32_MIN);
            ++mp->stage;
            continue;
        }

        case MP_EVASION :
        case MP_QCAPTURE :
            return select_any(mp);

        case MP_PROBCUT :
            return select_probcut(mp);

        default :
            return MOVE_NONE;
        }
    }
}
