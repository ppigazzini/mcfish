#include "search_main.h"

#include "movepick.h"
#include "search_back.h"
#include "search_common.h"
#include "search_control.h"
#include "search_qsearch.h"
#include "tb_source.h"
#include "tt.h"

#include "../board/bitboard.h"
#include "../board/repetition.h"
#include "../board/score.h"

// Select the ProbCut picker's opening stage the way upstream does: enter at the
// TT stage only when the TT move is a pseudo-legal capture, but keep `tt_move`
// set either way so the generated list still filters it out.
static void mp_set_probcut_stage(MovePicker *mp, const Position *pos, Move tt_move) {
    const bool usable = tt_move != MOVE_NONE && search_capture_stage(pos, tt_move)
                     && search_pseudo_legal(pos, tt_move);
    mp->stage = MP_PROBCUT_TT + (int) (!usable);
}

Value search_node(SearchCtx *ctx,
                  Position *pos,
                  Stack *ss,
                  Value alpha_in,
                  Value beta_in,
                  int depth_in,
                  bool cut_node,
                  NodeType nt) {
    const bool pv_node = nt_is_pv(nt);
    const bool root_node = nt_is_root(nt);
    const bool all_node = !(pv_node || cut_node);

    // Dive into qsearch at depth 0.
    if (depth_in <= 0)
        return qsearch_node(ctx, pos, ss, alpha_in, beta_in, pv_node);

    Histories *const h = ctx->hist;
    Stack *const ss1 = ss - 1;
    Stack *const ss2 = ss - 2;

    Value alpha = alpha_in;
    Value beta = beta_in;
    int depth = depth_in < MAX_PLY - 1 ? depth_in : MAX_PLY - 1;

    // Detect the upcoming-repetition draw (non-root).
    if (!root_node && alpha < VALUE_DRAW && pos_upcoming_repetition(pos, ss->ply)) {
        alpha = search_value_draw(ctx->nodes);
        if (alpha >= beta)
            return alpha;
    }

    StateInfo st;

    // Step 1. Initialize node.
    ss->in_check = checkers(pos) != 0;
    const bool prior_capture = captured_piece(pos) != NO_PIECE;
    const Color us = pos->side_to_move;
    ss->move_count = 0;
    Value best_value = -VALUE_INFINITE;
    Value max_value = VALUE_INFINITE;

    ss->follow_pv =
      root_node || (ss1->follow_pv && in_last_iter_pv(ctx, ss->ply - 1, ss1->current_move));

    check_time(ctx);

    if (pv_node)
        search_update_sel_depth(ctx, ss->ply);

    if (!root_node) {
        // Step 2. Bail on an aborted search / immediate draw / max ply.
        if (search_stopped(ctx) || pos_is_draw(pos, ss->ply) || ss->ply >= MAX_PLY) {
            if (ss->ply >= MAX_PLY && !ss->in_check)
                return search_evaluate(ctx, pos);
            return search_value_draw(ctx->nodes);
        }

        // Step 3. Prune by mate distance.
        alpha = alpha > mated_in(ss->ply) ? alpha : mated_in(ss->ply);
        beta = beta < mate_in(ss->ply + 1) ? beta : mate_in(ss->ply + 1);
        if (alpha >= beta)
            return alpha;
    }

    const int prev_sq =
      search_move_ok(ss1->current_move) ? (int) move_to(ss1->current_move) : (int) SQ_NONE;
    const int prior_reduction = ss1->reduction;
    ss1->reduction = 0;
    ss->stat_score = 0;
    (ss + 2)->cutoff_cnt = 0;

    // Step 4. Look up the transposition table.
    const Move excluded_move = ss->excluded_move;
    const Key pos_key = adjust_key50(pos);
    const TTProbe probe = search_tt_probe(pos_key);
    const bool tt_hit = probe.found;
    ss->tt_hit = tt_hit;
    const Move tt_move = root_node ? root_tt_move(ctx) : (tt_hit ? probe.move : MOVE_NONE);
    const Value tt_value =
      tt_hit ? search_value_from_tt(probe.value, ss->ply, pos->st->rule50) : VALUE_NONE;
    const int tt_depth = probe.depth;
    const Bound tt_bound_v = probe.bound;
    const Value tt_eval = probe.eval;
    const bool tt_is_pv = tt_hit && probe.is_pv;
    ss->tt_pv = excluded_move != MOVE_NONE ? ss->tt_pv : (pv_node || tt_is_pv);
    const bool tt_capture = tt_move != MOVE_NONE && search_capture_stage(pos, tt_move);
    TTEntry *const writer = probe.writer;

    // Step 5. Compute the static evaluation.
    Value unadjusted_static_eval = VALUE_NONE;
    const int correction_value = search_correction_value(h, pos, ss);
    Value eval;
    if (ss->in_check) {
        ss->static_eval = ss2->static_eval;
        eval = ss2->static_eval;
    } else if (excluded_move != MOVE_NONE) {
        unadjusted_static_eval = ss->static_eval;
        eval = ss->static_eval;
    } else if (ss->tt_hit) {
        unadjusted_static_eval = tt_eval;
        if (!value_is_valid(unadjusted_static_eval))
            unadjusted_static_eval = search_evaluate(ctx, pos);
        ss->static_eval = to_corrected_static_eval(unadjusted_static_eval, correction_value);
        eval = ss->static_eval;
        if (value_is_valid(tt_value)
            && (tt_bound_v & (tt_value > eval ? BOUND_LOWER : BOUND_UPPER)) != 0)
            eval = tt_value;
    } else {
        unadjusted_static_eval = search_evaluate(ctx, pos);
        ss->static_eval = to_corrected_static_eval(unadjusted_static_eval, correction_value);
        eval = ss->static_eval;
        search_tt_save(writer, pos_key, VALUE_NONE, ss->tt_pv, BOUND_NONE, DEPTH_UNSEARCHED,
                       MOVE_NONE, unadjusted_static_eval);
    }

    bool improving = ss->static_eval > ss2->static_eval;
    const bool opponent_worsening = ss->static_eval > -ss1->static_eval;

    // Apply the hindsight reduction adjustments.
    if (prior_reduction >= 3 && !opponent_worsening)
        depth += 1;
    if (prior_reduction >= 2 && depth >= 2 && ss->static_eval + ss1->static_eval > 173)
        depth -= 1;

    // Cut off early on the TT (non-PV).
    if (!pv_node && excluded_move == MOVE_NONE && tt_depth > depth - (int) (tt_value <= beta)
        && value_is_valid(tt_value)
        && (tt_bound_v & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != 0
        && (cut_node == (tt_value >= beta) || depth > 4)) {
        if (tt_move != MOVE_NONE && tt_value >= beta) {
            if (!tt_capture)  // upstream 73826352d
                search_update_quiet_histories(ctx, pos, ss, tt_move,
                                              114 * depth < 724 ? 114 * depth : 724);
            if (prev_sq != (int) SQ_NONE && ss1->move_count < 4 && !prior_capture)
                search_update_continuation_histories(ss1, piece_on(pos, (Square) prev_sq),
                                                     (Square) prev_sq, -2187);
        }
        if (pos->st->rule50 < 96) {
            if (depth >= 7 && tt_move != MOVE_NONE && search_pseudo_legal(pos, tt_move)
                && pos_legal(pos, tt_move) && !value_is_decisive(tt_value)) {
                pos_do_move(pos, tt_move, &st, search_gives_check(pos, tt_move), &pos->scratch_dp,
                            &pos->scratch_dts);
                const Key next_key = adjust_key50(pos);
                const TTProbe probe_next = search_tt_probe(next_key);
                pos_undo_move(pos, tt_move);
                const Value next_value = probe_next.found ? probe_next.value : VALUE_NONE;
                if (!value_is_valid(next_value))
                    return tt_value;
                if ((tt_value >= beta) == (-next_value >= beta))
                    return tt_value;
            } else {
                return tt_value;
            }
        }
    }
    // upstream 319d61eff: take no cutoff, but if a window-bound mismatch is the
    // only reason, penalize the now-useless entry (decrement its stored depth).
    else if (!pv_node && excluded_move == MOVE_NONE && tt_depth > depth - (int) (tt_value <= beta)
             && value_is_valid(tt_value) && tt_bound_v != (BOUND_LOWER | BOUND_UPPER)
             && (tt_bound_v & (tt_value >= beta ? BOUND_UPPER : BOUND_LOWER)) != 0 && depth > 5) {
        search_tt_penalize(writer, 1);
    }

    // Step 6. Probe the tablebases. Probe the WDL of the current (non-root,
    // non-excluded) position when it is small enough, has a zeroed rule50 counter
    // and no castling rights; on success score it into the VALUE_TB..VALUE_TB_WIN
    // range and cut or adjust. Gated on tb_config.cardinality, which is 0 without
    // a SyzygyPath, so a default build never enters here.
    if (!root_node && excluded_move == MOVE_NONE) {
        const int cardinality = ctx->tb_config.cardinality;
        if (cardinality != 0) {
            const int pieces_count = popcount_bb(pieces(pos));
            const int probe_depth = ctx->tb_config.probe_depth;
            if (pieces_count <= cardinality && (pieces_count < cardinality || depth >= probe_depth)
                && pos->st->rule50 == 0 && pos->st->castling_rights == 0) {
                const TbProbeResult res = TbProbeWdlPos(pos);
                if (res.available != 0) {
                    ctx->tb_hits += 1;
                    const int draw_score = ctx->tb_config.use_rule50 ? 1 : 0;
                    const Value tb_value = (Value) (VALUE_TB - ss->ply);
                    const int wdl = res.wdl;
                    const Value value = wdl < -draw_score ? (Value) -tb_value
                                      : wdl > draw_score
                                        ? tb_value
                                        : (Value) (VALUE_DRAW + 2 * wdl * draw_score);
                    const Bound b = wdl < -draw_score ? BOUND_UPPER
                                  : wdl > draw_score  ? BOUND_LOWER
                                                      : BOUND_EXACT;
                    if (b == BOUND_EXACT || (b == BOUND_LOWER ? value >= beta : value <= alpha)) {
                        search_tt_save(
                          writer, pos_key, search_value_to_tt(value, ss->ply), ss->tt_pv, b,
                          depth + 6 < MAX_PLY - 1 ? depth + 6 : MAX_PLY - 1, MOVE_NONE, VALUE_NONE);
                        return value;
                    }
                    if (pv_node) {
                        if (b == BOUND_LOWER) {
                            best_value = value;
                            alpha = alpha > best_value ? alpha : best_value;
                        } else {
                            max_value = value;
                        }
                    }
                }
            }
        }
    }

    if (!ss->in_check) {
        // Order quiets by static-eval difference.
        if (search_move_ok(ss1->current_move) && !ss1->in_check && !prior_capture) {
            const int diff = eval_diff(ss1->static_eval, ss->static_eval);
            stats_update(
              &h->main_history[(size_t) flip_color(us) * HIST_UINT16 + (size_t) ss1->current_move],
              diff * 10, 7183);
            if (!tt_hit && type_of_piece(piece_on(pos, (Square) prev_sq)) != PAWN
                && move_type(ss1->current_move) != PROMOTION) {
                const Square psq = (Square) prev_sq;
                int16_t *const row = pawn_history_row(h, pos->st->pawn_key);
                stats_update(&row[(size_t) piece_on(pos, psq) * SQUARE_NB + (size_t) psq],
                             diff * 13, 8192);
            }
        }

        // Step 7. Apply razoring.
        if (!pv_node && eval < alpha - razor_margin(depth))
            return qsearch_node(ctx, pos, ss, alpha, beta, false);

        // Step 8. Prune by futility.
        if (!ss->tt_pv && depth < 17 && eval >= beta && (tt_move == MOVE_NONE || tt_capture)
            && !value_is_loss(beta) && !value_is_win(eval)) {
            const int fm =
              futility_margin(depth, ss->tt_hit, improving, opponent_worsening, correction_value);
            if (eval - fm >= beta)
                return (Value) futility_return(beta, eval);
        }

        // Step 9. Search the null move.
        if (cut_node && ss->static_eval >= null_move_threshold(beta, depth, improving)
            && excluded_move == MOVE_NONE && pos_non_pawn_material(pos, us) != 0
            && ss->ply >= ctx->nmp_min_ply && !value_is_loss(beta)) {
            const int r = null_move_reduction(depth);
            // Touch no accumulator for a null move: mark the stack move as null and
            // install the all-NO_PIECE continuation pages.
            pos_do_null_move(pos, &st, &pos->scratch_dp, &pos->scratch_dts);
            ss->current_move = MOVE_NULL;
            search_set_cont_hist(ctx, ss, false, false, NO_PIECE, SQ_A1);
            const Value null_value =
              (Value) -search_node(ctx, pos, ss + 1, -beta, -beta + 1, depth - r, false, NT_NON_PV);
            pos_undo_null_move(pos);
            if (null_value >= beta && !value_is_win(null_value)) {
                if (ctx->nmp_min_ply != 0 || depth < 16)
                    return null_value;
                ctx->nmp_min_ply = nmp_min_ply_of(ss->ply, depth, r);
                const Value v =
                  search_node(ctx, pos, ss, beta - 1, beta, depth - r, false, NT_NON_PV);
                ctx->nmp_min_ply = 0;
                if (v >= beta)
                    return null_value;
            }
        }

        if (ss->static_eval >= beta)
            improving = true;

        // Step 10. Apply internal iterative reductions.
        // upstream b1053e60b: drop the priorReduction <= 3 term.
        if (!ss->follow_pv && !all_node && depth >= 6 && tt_move == MOVE_NONE)
            depth -= 1;

        // Step 11. Run ProbCut.
        const int pc_beta = probcut_beta(beta, improving);
        if (depth >= 3 && !value_is_decisive(beta)
            && !(value_is_valid(tt_value) && tt_value < pc_beta)) {
            MovePicker mp;
            movepick_init_probcut(&mp, pos, h, tt_move, pc_beta - ss->static_eval);
            mp_set_probcut_stage(&mp, pos, tt_move);

            const int probcut_depth = depth - 4 - (int) improving;  // upstream d64835051
            for (Move move = movepick_next(&mp); move != MOVE_NONE; move = movepick_next(&mp)) {
                if (move == excluded_move || !pos_legal(pos, move))
                    continue;
                search_do_move(ctx, pos, move, &st, search_gives_check(pos, move), ss);
                Value value =
                  (Value) -qsearch_node(ctx, pos, ss + 1, -pc_beta, -pc_beta + 1, false);
                if (value >= pc_beta && probcut_depth > 0)
                    value = (Value) -search_node(ctx, pos, ss + 1, -pc_beta, -pc_beta + 1,
                                                 probcut_depth, !cut_node, NT_NON_PV);
                search_undo_move(ctx, pos, move);
                if (value >= pc_beta) {
                    search_tt_save(writer, pos_key, search_value_to_tt(value, ss->ply), ss->tt_pv,
                                   BOUND_LOWER, probcut_depth + 1, move, unadjusted_static_eval);
                    if (!value_is_decisive(value))
                        return (Value) (value - (pc_beta - beta));
                }
            }
        }
    }

    // moves_loop:
    // Step 12. Apply the deep-ProbCut TT idea.
    const int pc_beta_deep = probcut_beta_deep(beta);
    if ((tt_bound_v & BOUND_LOWER) != 0 && tt_depth >= depth - 4 && tt_value >= pc_beta_deep
        && !value_is_decisive(beta) && value_is_valid(tt_value) && !value_is_decisive(tt_value))
        return (Value) pc_beta_deep;

    const SearchNodeState nd = {
        .ctx = ctx,
        .pos = pos,
        .ss = ss,
        .ss1 = ss1,
        .hist = h,
        .us = us,
        .alpha = alpha,
        .beta = beta,
        .depth = depth,
        .best_value = best_value,
        .max_value = max_value,
        .excluded_move = excluded_move,
        .tt_move = tt_move,
        .tt_value = tt_value,
        .tt_depth = tt_depth,
        .tt_bound = tt_bound_v,
        .tt_capture = tt_capture,
        .correction_value = correction_value,
        .cut_node = cut_node,
        .pv_node = pv_node,
        .root_node = root_node,
        .all_node = all_node,
        .improving = improving,
        .unadjusted_static_eval = unadjusted_static_eval,
        .writer = writer,
        .pos_key = pos_key,
        .prev_sq = prev_sq,
        .prior_capture = prior_capture,
    };
    return search_run_back(&nd);
}
