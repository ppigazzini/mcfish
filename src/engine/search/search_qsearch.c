#include "search_qsearch.h"
#include "../board/legality.h"

#include "movepick.h"
#include "search_common.h"
#include "tt.h"

#include "../board/movegen.h"
#include "../board/repetition.h"
#include "../board/score.h"

#include <string.h>

void pv_update(PVMoves *pv, Move move, const PVMoves *child) {
    const size_t n = child ? child->length : 0;
    for (size_t i = 0; i < n; ++i)
        pv->moves[i + 1] = child->moves[i];
    pv->moves[0] = move;
    pv->length = n + 1;
}

bool is_shuffling(const Position *pos, const Stack *ss, Move move) {
    if (search_capture_stage(pos, move) || pos->st->rule50 < 10)
        return false;
    if (pos->st->plies_from_null < 6 || ss->ply < 20)
        return false;

    const Stack *const ss2 = ss - 2;
    const Stack *const ss4 = ss - 4;
    return move_from(move) == move_to(ss2->current_move)
        && move_from(ss2->current_move) == move_to(ss4->current_move);
}

Key adjust_key50(const Position *pos) {
    const Key k = pos->st->key;
    if (pos->st->rule50 < 14)
        return k;
    const Key seed = (Key) ((pos->st->rule50 - 14) / 8);
    return k ^ (seed * 6364136223846793005ULL + 1442695040888963407ULL);
}

void tt_move_history_update(Histories *h, int bonus) {
    stats_update(&h->tt_move_history, bonus, 8192);
}

// Select the picker's opening stage the way upstream does: skip the TT stage when
// there is no usable TT move, but keep `tt_move` set either way so the generated
// list still filters it out.
static void mp_set_main_stage(MovePicker *mp, const Position *pos, Move tt_move, int depth) {
    const int base = checkers(pos) != 0 ? MP_EVASION_TT : depth > 0 ? MP_MAIN_TT : MP_QSEARCH_TT;
    const bool usable = tt_move != MOVE_NONE && search_pseudo_legal(pos, tt_move);
    mp->stage = base + (int) (!usable);
}

Value qsearch_node(
  SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, bool pv_node) {
    Histories *const h = ctx->hist;
    Stack *const ss1 = ss - 1;
    Stack *const ss_next = ss + 1;

    // Detect the upcoming-repetition draw.
    if (alpha < VALUE_DRAW && pos_upcoming_repetition(pos, ss->ply)) {
        alpha = search_value_draw(ctx_nodes(ctx));
        if (alpha >= beta)
            return alpha;
    }

    PVMoves pv;
    StateInfo st;

    Move best_move = MOVE_NONE;
    ss->in_check = checkers(pos) != 0;
    int move_count = 0;

    // Step 1. Initialize node (PV).
    if (pv_node) {
        ss_next->pv = &pv;
        pv_clear(ss->pv);
        search_update_sel_depth(ctx, ss->ply);
    }

    // Step 2. Return on an immediate draw or at max ply.
    if (pos_is_draw(pos, ss->ply) || ss->ply >= MAX_PLY) {
        if (ss->ply >= MAX_PLY && !ss->in_check)
            return search_evaluate(ctx, pos);
        return VALUE_DRAW;
    }

    // Step 3. Look up the transposition table.
    const Key pos_key = adjust_key50(pos);
    const TTProbe probe = search_tt_probe(pos_key);
    const bool tt_hit = probe.found;
    ss->tt_hit = tt_hit;
    const Move tt_move = tt_hit ? probe.move : MOVE_NONE;
    const Value tt_value =
      tt_hit ? search_value_from_tt(probe.value, ss->ply, pos->st->rule50) : VALUE_NONE;
    const int tt_depth = probe.depth;
    const Bound tt_bound_v = probe.bound;
    const Value tt_eval = probe.eval;
    const bool pv_hit = tt_hit && probe.is_pv;
    TTEntry *const writer = probe.writer;

    if (!pv_node && tt_depth >= DEPTH_QS && value_is_valid(tt_value)
        && (tt_bound_v & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != 0)
        return tt_value;

    // Step 4. Compute the static evaluation.
    Value unadjusted_static_eval = VALUE_NONE;
    Value best_value;
    int futility_base = -VALUE_INFINITE;
    if (ss->in_check) {
        best_value = -VALUE_INFINITE;
    } else {
        const int correction_value = search_correction_value(h, pos, ss);
        if (ss->tt_hit) {
            unadjusted_static_eval = tt_eval;
            if (!value_is_valid(unadjusted_static_eval))
                unadjusted_static_eval = search_evaluate(ctx, pos);
            ss->static_eval = to_corrected_static_eval(unadjusted_static_eval, correction_value);
            best_value = ss->static_eval;
            if (value_is_valid(tt_value) && !value_is_decisive(tt_value)
                && (tt_bound_v & (tt_value > best_value ? BOUND_LOWER : BOUND_UPPER)) != 0)
                best_value = tt_value;
        } else {
            unadjusted_static_eval = search_evaluate(ctx, pos);
            ss->static_eval = to_corrected_static_eval(unadjusted_static_eval, correction_value);
            best_value = ss->static_eval;
        }

        // Stand pat.
        if (best_value >= beta) {
            if (!value_is_decisive(best_value))
                best_value = qsearch_stand_pat_blend(best_value, beta);
            if (!ss->tt_hit)
                search_tt_save(writer, pos_key, VALUE_NONE, false, BOUND_LOWER, DEPTH_UNSEARCHED,
                               MOVE_NONE, unadjusted_static_eval);
            return best_value;
        }
        if (best_value > alpha)
            alpha = best_value;
        futility_base = qsearch_futility_base(ss->static_eval);
    }

    const int prev_sq =
      search_move_ok(ss1->current_move) ? (int) move_to(ss1->current_move) : (int) SQ_NONE;

    // Step 5. Pick moves (captures, or evasions when in check).
    MovePicker mp;
    movepick_init(&mp, pos, h, pos->st->pawn_key, tt_move, DEPTH_QS, ss->ply, ss);
    mp_set_main_stage(&mp, pos, tt_move, DEPTH_QS);

    for (Move move = movepick_next(&mp); move != MOVE_NONE; move = movepick_next(&mp)) {
        if (!pos_legal(pos, move))
            continue;

        const bool gc = search_gives_check(pos, move);
        const bool capture = search_capture_stage(pos, move);
        move_count += 1;

        // Step 6. Prune.
        if (!value_is_loss(best_value)) {
            if (!gc && (int) move_to(move) != prev_sq && !value_is_loss(futility_base)
                && move_type(move) != PROMOTION) {
                if (move_count > 2)
                    continue;
                const int futility_value =
                  futility_base + PieceValueByPiece[piece_on(pos, move_to(move))];
                if (futility_value <= alpha) {
                    if (futility_value > best_value)
                        best_value = futility_value;
                    continue;
                }
                if (!see_ge(pos, move, alpha - futility_base)) {
                    const int cap = alpha < futility_base ? alpha : futility_base;
                    if (cap > best_value)
                        best_value = cap;
                    continue;
                }
            }
            if (!capture)
                continue;
            if (!see_ge(pos, move, -74))
                continue;
        }

        // Step 7. Make and search the move.
        search_do_move(ctx, pos, move, &st, gc, ss);
        const Value value = (Value) -qsearch_node(ctx, pos, ss_next, -beta, -alpha, pv_node);
        search_undo_move(ctx, pos, move);

        // Step 8. Record a new best move.
        if (value > best_value) {
            best_value = value;
            if (value > alpha) {
                best_move = move;
                if (pv_node)
                    pv_update(ss->pv, move, ss_next->pv);
                if (value < beta)
                    alpha = value;
                else
                    break;
            }
        }
    }

    // Step 9. Detect mate / stalemate.
    if (move_count == 0) {
        if (ss->in_check)
            return mated_in(ss->ply);
        const Color us = pos->side_to_move;
        const Bitboard pawns = pieces_cp(pos, us, PAWN);
        const Bitboard pushed = us == WHITE ? pawns << 8 : pawns >> 8;
        if ((pushed & ~pieces(pos)) == 0 && pos_non_pawn_material(pos, us) == 0
            && type_of_piece(captured_piece(pos)) >= KNIGHT) {
            ExtMove lbuf[MAX_MOVES];
            if (generate_legal(pos, lbuf) == lbuf)
                best_value = VALUE_DRAW;
        }
    }

    if (!value_is_decisive(best_value) && best_value > beta)
        best_value = qsearch_fail_high_blend(best_value, beta);

    // Save to the transposition table.
    search_tt_save(writer, pos_key, search_value_to_tt(best_value, ss->ply), pv_hit,
                   best_value >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, best_move,
                   unadjusted_static_eval);

    return best_value;
}
