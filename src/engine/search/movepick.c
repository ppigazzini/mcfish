#include "movepick.h"

#include "../board/legality.h"

#include "history.h"

#include "../board/attacks.h"
#include "../board/bitboard.h"
#include "../board/movegen.h"
#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __AVX512F__
    #include <immintrin.h>
#endif

// Index the piece values by PIECE, not by PieceType: the capture and evasion
// scorers read `PieceValue[pos.piece_on(to))]` straight off the board, so the
// table repeats for the black half and leaves the two encoding gaps at 0.
static constexpr int PieceValues[PIECE_NB] = {
    0, 208, 781, 825, 1276, 2538, 0, 0, 0, 208, 781, 825, 1276, 2538, 0, 0,
};

enum { KIND_CAPTURES = 0, KIND_QUIETS = 1, KIND_EVASIONS = 2 };

enum { GOOD_QUIET_THRESHOLD = -14000 };

// Return the squares from which PT gives check to the side not to move.
// Read the check squares cached by set_check_info (position.c). check_squares[KING]
// is 0, so a KING mover (castling is encoded king-captures-rook) reports no direct
// check, as upstream's zero KING entry does.
static Bitboard check_squares(const Position *pos, PieceType pt) {
    return pos->st->check_squares[pt];
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

// Read one continuation plane at an ALREADY-FLATTENED element index. The six history
// planes a quiet move reads are all indexed at the same [pc][to], and each plane is one
// run of HIST_PIECETO entries, so pc * SQUARE_NB + to names the element directly. Taking
// the flat index as the argument lets the caller compute it once instead of six times:
// clang lowers `row[pc * SQUARE_NB + to]` as `row + pc * 128` plus an index of `to * 2`,
// so the addressing mode carries one term and an add carries the other, once per plane.
static inline int cont_hist_score(const MovePicker *mp, size_t slot, size_t hi) {
    return shared_stat_load(&mp->cont_hist[slot][hi]);
}

// Generate the KIND move list into OUT and fill each entry's ordering value.
// Return the number of moves written.
static size_t score_list(const MovePicker *mp, int kind, ExtMove *out) {
    const Position *pos = mp->pos;
    Histories *h = mp->hist;

    ExtMove *const end = kind == KIND_CAPTURES ? generate_captures(pos, out)
                       : kind == KIND_QUIETS   ? generate_quiets(pos, out)
                                               : generate_evasions(pos, out);
    const size_t count = (size_t) (end - out);

    const Color us = pos->side_to_move;

    // Collect, per moving piece type, the squares attacked by a strictly cheaper
    // enemy piece. Quiet scoring pays for stepping into one and rewards leaving one.
    Bitboard threat_by_lesser[PIECE_TYPE_NB] = { 0 };
    // Hoisted out of the move loop: the row is a function of the pawn key alone, which
    // does not change while one position's move list is scored, and the lookup masks and
    // scales the key. It was paid once per QUIET move.
    const SharedStat *pawn_row = nullptr;
    if (kind == KIND_QUIETS) {
        const Color them = flip_color(us);
        threat_by_lesser[PAWN] = 0;
        threat_by_lesser[KNIGHT] = attacks_by(pos, them, PAWN);
        threat_by_lesser[BISHOP] = threat_by_lesser[KNIGHT];
        threat_by_lesser[ROOK] =
          attacks_by(pos, them, KNIGHT) | attacks_by(pos, them, BISHOP) | threat_by_lesser[KNIGHT];
        threat_by_lesser[QUEEN] = attacks_by(pos, them, ROOK) | threat_by_lesser[ROOK];
        threat_by_lesser[KING] = 0;

        pawn_row = pawn_history_row(h, mp->pawn_key);
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
            const size_t hi = (size_t) pc * SQUARE_NB + (size_t) to;

            const int main_history = h->main_history[(size_t) us * HIST_UINT16 + (size_t) m];
            const int pawn_history = shared_stat_load(&pawn_row[hi]);
            const int continuation_sum = cont_hist_score(mp, 0, hi) + cont_hist_score(mp, 1, hi)
                                       + cont_hist_score(mp, 2, hi) + cont_hist_score(mp, 3, hi)
                                       + cont_hist_score(mp, 5, hi);

            // Both threat tests are the same bit test on the same bitboard, but
            // `b & square_bb(s)` hands clang a VALUE to test where a shift hands it a bit
            // POSITION: from the `to` half it builds `1 << to` and tests that, where the
            // `from` half it already lowers to a `bt`. Written as shifts both are `bt`
            // and the difference falls out of the carry.
            const Bitboard threat = threat_by_lesser[pt];
            const int threat_term = (int) (threat >> from & 1) - (int) (threat >> to & 1);

            value = 2 * main_history + 2 * pawn_history + continuation_sum
                  + PieceValues[pt] * 20 * threat_term;

            // A statement rather than a multiply by the predicate. The short circuit is
            // already a branch, and the product makes its NOT-taken arm materialise a
            // zero and add it -- and that arm is almost every move scored, since the
            // check-square test in front of see_ge is false for the overwhelming
            // majority of quiets.
            if ((check_squares(pos, pt) & square_bb(to)) != 0 && see_ge(pos, m, -75))
                value += 16384;
        } else {
            if (capture_stage(pos, m)) {
                value = PieceValues[captured] + (1 << 28);
            } else {
                value = h->main_history[(size_t) us * HIST_UINT16 + (size_t) m]
                      + cont_hist_score(mp, 0, (size_t) pc * SQUARE_NB + (size_t) to);
            }
        }

        out[i].value = value;
    }

    // THE LOW-PLY TERM IS A SECOND PASS over the same list, and the reason is the loop
    // above rather than this one. `ply`, the row it selects and the divisor it forms
    // are all invariant across a move list, but see_ge a few lines up is opaque, so
    // the compiler may not hoist any of them across it and re-derived all three per
    // move. Here there is no call: the row address and the divisor are formed once,
    // and a list scored at a ply past the table pays no test at all.
    //
    // Each move's term is the same value added to the same accumulator with nothing
    // between, so the sum is identical -- the signature is what says so.
    if (kind == KIND_QUIETS && mp->ply < LOW_PLY_HISTORY_SIZE) {
        const int16_t *const low_ply_row = &h->low_ply_history[(size_t) mp->ply * HIST_UINT16];
        const int divisor = 1 + mp->ply;
        for (size_t i = 0; i < count; ++i) {
            out[i].value += 8 * (int) low_ply_row[(size_t) out[i].move] / divisor;
        }
    }

    return count;
}

// Sort the entries whose value is at least LIMIT to the front, in descending
// order, leaving the rest where they are. Entry 0 is the initial sorted head and
// is never tested against LIMIT, exactly as upstream's partial_insertion_sort.
#ifdef __AVX512F__
// Sort the first up-to-16 qualifying moves in two 512-bit registers, as upstream's
// MoveSorter does (movepick.cpp:66). Values and moves live in SEPARATE registers so
// an insertion is one masked expand each; `write_sorted` reassembles the 8-byte
// ExtMoves with a permute across the register pair.
//
// The ORDER this produces is the scalar order, exactly -- a difference of one swap
// changes the move loop and therefore the tree, which is what ./build.sh signature
// and arch-determinism gate. mcfish's ExtMove is 8 bytes with `move` at 0 and
// `value` at 4, matching what the reassembly below assumes.
enum { MOVE_SORTER_MAX = 16 };

typedef struct {
    __m512i sorted_values;
    __m512i sorted_moves;
} MoveSorter;

static void splat_extmove(ExtMove m, __m512i *move, __m512i *value) {
    *move = _mm512_set1_epi32((int) (unsigned) m.move);
    *value = _mm512_set1_epi32(m.value);
}

static MoveSorter move_sorter_init(ExtMove first) {
    MoveSorter s;
    splat_extmove(first, &s.sorted_moves, &s.sorted_values);
    // Every lane but the first sorts below any real move.
    s.sorted_values = _mm512_mask_set1_epi32(s.sorted_values, (__mmask16) ~1, INT32_MIN);
    return s;
}

static void move_sorter_insert(MoveSorter *s, ExtMove m) {
    __m512i move, value;
    splat_extmove(m, &move, &value);

    // Mask of every element except the insertion point.
    const __mmask16 expand =
      _kadd_mask16(_mm512_cmplt_epi32_mask(s->sorted_values, value), (__mmask16) -1);

    s->sorted_values = _mm512_mask_expand_epi32(value, expand, s->sorted_values);
    s->sorted_moves = _mm512_mask_expand_epi32(move, expand, s->sorted_moves);
}

static void move_sorter_write(const MoveSorter *s, ExtMove *moves, size_t count) {
    static_assert(sizeof(ExtMove) == 8, "the reassembly below packs two 32-bit lanes per move");

    // Values and moves are held apart, so interleave them back into ExtMoves.
    const __m512i lo = _mm512_setr_epi32(0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
    const __m512i hi =
      _mm512_setr_epi32(8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);

    for (size_t offset = 0; offset < 16; offset += 8) {
        if (count <= offset)
            break;
        const __m512i ext =
          _mm512_permutex2var_epi32(s->sorted_moves, offset == 0 ? lo : hi, s->sorted_values);
        const size_t store_count = count - offset;
        _mm512_mask_storeu_epi64(moves + offset, (__mmask8) ((1u << store_count) - 1u), ext);
    }
}
#endif

// Sort every entry into descending order. Two of the three call sites pass a limit
// no score can fail -- INT32_MIN -- and under it the general form above degenerates:
// `sorted_end` advances on every move, so it tracks `scan` exactly and
// `entries[scan] = entries[++sorted_end]` copies a slot onto itself. clang emits that
// copy: it reloads the eight bytes and stores them back to the address it read them
// from, once per move, and pays the limit test beside it.
//
// The order out is the order in: the ladder starts at `scan`, which is the slot
// `sorted_end` names in the general form, and the vector prefix takes the same
// first min(count, MOVE_SORTER_MAX) moves. ./build.sh signature is what says so --
// one swap of difference moves the move loop and therefore the tree.
static void sort_all(ExtMove *entries, size_t count) {
    if (count == 0)
        return;

    size_t scan = 1;

#ifdef __AVX512F__
    // The general form breaks out when the sorter is full, which under an admitting
    // limit is exactly `scan == MOVE_SORTER_MAX`.
    const size_t vector_count = count < (size_t) MOVE_SORTER_MAX ? count : (size_t) MOVE_SORTER_MAX;
    MoveSorter sorter = move_sorter_init(entries[0]);
    for (; scan < vector_count; ++scan)
        move_sorter_insert(&sorter, entries[scan]);
    move_sorter_write(&sorter, entries, vector_count);
#endif

    for (; scan < count; ++scan) {
        const ExtMove current = entries[scan];

        size_t insert_at = scan;
        while (insert_at != 0 && entries[insert_at - 1].value < current.value) {
            entries[insert_at] = entries[insert_at - 1];
            --insert_at;
        }
        entries[insert_at] = current;
    }
}

static void partial_insertion_sort(ExtMove *entries, size_t count, int limit) {
    if (count == 0)
        return;

    size_t sorted_end = 0;
    size_t scan = 1;

#ifdef __AVX512F__
    // Vector pass over the leading run, then the scalar loop finishes the tail --
    // upstream's shape at movepick.cpp:114.
    MoveSorter sorter = move_sorter_init(entries[0]);
    for (; scan < count; ++scan) {
        if (entries[scan].value >= limit) {
            if (sorted_end + 1 >= (size_t) MOVE_SORTER_MAX)  // sorter full
                break;
            move_sorter_insert(&sorter, entries[scan]);
            entries[scan] = entries[++sorted_end];
        }
    }
    move_sorter_write(&sorter, entries, sorted_end + 1);
#endif

    for (; scan < count; ++scan) {
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
    mp->cont_hist = nullptr;
    mp->ply = 0;
    mp->tt_move = tt_move;
    mp->threshold = 0;
    mp->depth = 0;
    mp->skip_quiets = false;
    // Leave the cursor/span fields (cur, end_cur, end_bad_captures, end_captures,
    // end_generated) unset, as upstream's constructor does: every stage of
    // movepick_next writes each one before any stage reads it, and movepick_next
    // is an out-of-line call, so a zero-fill here is five stores per picker the
    // optimizer cannot prove dead.
}

void movepick_init(MovePicker *mp,
                   const Position *pos,
                   Histories *h,
                   Key pawn_key,
                   Move tt_move,
                   int depth,
                   int ply,
                   const SharedStat *const *cont_hist) {
    init_common(mp, pos, h, tt_move);
    mp->pawn_key = pawn_key;
    mp->cont_hist = cont_hist;
    mp->ply = ply;
    mp->depth = depth;
}

void movepick_init_probcut(
  MovePicker *mp, const Position *pos, Histories *h, Move tt_move, int threshold) {
    init_common(mp, pos, h, tt_move);
    mp->threshold = threshold;
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

// Walk for the next quiet scoring above GOOD_QUIET_THRESHOLD, stopping at the first
// move that does not.
//
// partial_insertion_sort leaves the list in two pieces: a prefix that DESCENDS, and a
// tail every member of which scores below the sort's own limit. So past the first move
// at or below the threshold there is nothing left to find -- the rest of the prefix is
// at or below it by the ordering, and the tail is below it by the limit.
//
// The tail half of that argument holds only while the LIMIT is at or below the
// threshold. The limit is -3560 * depth and reaches -14000 at depth 4, so below that a
// tail move can still outscore the threshold and the walk has to run to the end. The
// caller picks between the two forms on exactly that test.
static Move select_good_quiet_bounded(MovePicker *mp) {
    while (mp->cur < mp->end_cur) {
        const ExtMove entry = mp->moves[mp->cur];
        if (entry.value <= GOOD_QUIET_THRESHOLD)
            return MOVE_NONE;
        ++mp->cur;
        if (entry.move != mp->tt_move)
            return entry.move;
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
        // Shared by the three hoisted walks below, which a `goto` enters: a declaration
        // beside each one would sit past the switch, where nothing falls through to it.
        Move walk_move;

        // Essentially every indirect mispredict this engine pays is the switch below.
        // Over a warm game the dispatch is entered several times a node, and more than
        // half of those entries ask for one of the three CONSECUTIVE stages that do
        // nothing but walk a list -- GOOD_QUIET, BAD_CAPTURE, BAD_QUIET. Hoist those
        // three out of the switch and reach them by a range test, so the table keeps
        // only the twelve stages that generate, score or sort. They still chain by
        // fallthrough exactly as they did as cases; only QUIET_INIT reaches the first
        // of them by a jump rather than by falling in.
        //
        // Naming them AHEAD of the switch while they were still cases of it does
        // nothing: clang folds a test whose target is a case label back into the jump
        // table and emits a byte-identical dispatch. They have to LEAVE the switch for
        // the test to survive, which is why this is a move and not two lines.
        if ((unsigned) (mp->stage - MP_GOOD_QUIET) <= (unsigned) (MP_BAD_QUIET - MP_GOOD_QUIET)) {
            if (mp->stage == MP_GOOD_QUIET)
                goto good_quiet;
            if (mp->stage == MP_BAD_CAPTURE)
                goto bad_capture;
            goto bad_quiet;
        }

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
            sort_all(mp->moves + mp->cur, count);
            ++mp->stage;
            // Re-dispatch, as upstream's `goto top` does here (movepick.cpp:305): three
            // stages share this block and each has a different successor, so the
            // successor has to be read back off the stage. Every OTHER transition below
            // has exactly one successor and falls through to it.
            continue;
        }

        case MP_GOOD_CAPTURE : {
            const Move m = select_good_capture(mp);
            if (m != MOVE_NONE)
                return m;

            ++mp->stage;
            // Fall into the one successor this stage has, as upstream does
            // (movepick.cpp:317): re-dispatching would reload the stage and jump
            // through the switch's table, and that indirect jump is the one the
            // hardware predicts worst -- measured 48% mispredicted here.
            [[fallthrough]];
        }

        case MP_QUIET_INIT : {
            if (!mp->skip_quiets) {
                const size_t count = score_list(mp, KIND_QUIETS, mp->moves + mp->cur);

                mp->end_cur = mp->cur + count;
                mp->end_generated = mp->end_cur;
                partial_insertion_sort(mp->moves + mp->cur, count, -3560 * mp->depth);
            }

            ++mp->stage;
            goto good_quiet;
        }

        case MP_EVASION_INIT : {
            mp->cur = 0;

            const size_t count = score_list(mp, KIND_EVASIONS, mp->moves + mp->cur);

            mp->end_cur = mp->cur + count;
            mp->end_generated = mp->end_cur;
            sort_all(mp->moves + mp->cur, count);
            ++mp->stage;
            [[fallthrough]];
        }

        case MP_EVASION :
        case MP_QCAPTURE :
            return select_any(mp);

        case MP_PROBCUT :
            return select_probcut(mp);

        default :
            return MOVE_NONE;
        }

        // Unreachable: every case above returns, continues or jumps. The three hoisted
        // stages live here, past the switch, and are entered only through the range test
        // at the top of the loop or through the jump out of QUIET_INIT.
good_quiet:
        if (!mp->skip_quiets) {
            walk_move = -3560 * mp->depth <= GOOD_QUIET_THRESHOLD ? select_good_quiet_bounded(mp)
                                                                  : select_good_quiet(mp);
            if (walk_move != MOVE_NONE)
                return walk_move;
        }

        mp->cur = 0;
        mp->end_cur = mp->end_bad_captures;
        ++mp->stage;

bad_capture:
        walk_move = select_any(mp);
        if (walk_move != MOVE_NONE)
            return walk_move;

        mp->cur = mp->end_captures;
        mp->end_cur = mp->end_generated;
        ++mp->stage;

bad_quiet:
        if (!mp->skip_quiets)
            return select_bad_quiet(mp);
        return MOVE_NONE;
    }
}
