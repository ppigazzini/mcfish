#include "search_main.h"

#include "../board/legality.h"

#include "movepick.h"
#include "search_common.h"
#include "search_control.h"
#include "search_emit.h"
#include "search_qsearch.h"
#include "tb_source.h"
#include "tt.h"

#include "../board/bitboard.h"
#include "../board/repetition.h"
#include "../board/score.h"

// Enter the picker at the TT stage only when the TT move is usable, but keep
// `tt_move` set either way so the generated list still filters it out.
static void mp_set_main_stage(MovePicker *mp, const Position *pos, Move tt_move, int depth) {
    const int base = checkers(pos) != 0 ? MP_EVASION_TT : depth > 0 ? MP_MAIN_TT : MP_QSEARCH_TT;
    const bool usable = tt_move != MOVE_NONE && search_pseudo_legal(pos, tt_move);
    mp->stage = base + (int) (!usable);
}

// Select the ProbCut picker's opening stage the way upstream does: enter at the
// TT stage only when the TT move is a pseudo-legal capture, but keep `tt_move`
// set either way so the generated list still filters it out.
static void mp_set_probcut_stage(MovePicker *mp, const Position *pos, Move tt_move) {
    const bool usable = tt_move != MOVE_NONE && search_capture_stage(pos, tt_move)
                     && search_pseudo_legal(pos, tt_move);
    mp->stage = MP_PROBCUT_TT + (int) (!usable);
}

// Clone the node body per NodeType, the way upstream instantiates
// search<NonPV>/search<PV>/search<Root>: NT is a literal in every clone, so every
// pv_node / root_node test below -- including the ones in the move loop -- folds
// at compile time instead of running at every node. `always_inline` is what makes
// the clone happen at all: the cost model refuses a body this size, and a body
// that is emitted once with NT live is the shape this exists to avoid. The
// recursion goes through the exported clones below, never through this body,
// for the same reason qsearch's does.
__attribute__((always_inline)) static inline Value search_node_impl(SearchCtx *ctx,
                                                                    Position *pos,
                                                                    Stack *ss,
                                                                    Value alpha_in,
                                                                    Value beta_in,
                                                                    int depth_in,
                                                                    bool cut_node,
                                                                    const NodeType nt) {
    const bool pv_node = nt_is_pv(nt);
    const bool root_node = nt_is_root(nt);
    const bool all_node = !(pv_node || cut_node);

    // Dive into qsearch at depth 0.
    if (depth_in <= 0)
        return pv_node ? qsearch_node_pv(ctx, pos, ss, alpha_in, beta_in)
                       : qsearch_node_nonpv(ctx, pos, ss, alpha_in, beta_in);

    // Is the ROOT already hunting a mate? Upstream 598ae2c4 asks this once per node
    // and lets two readers below stand down on it: the futility cutoff drops from 19
    // to 6, and the singular extension is skipped entirely, so the tree collapses
    // onto the mating line instead of re-proving the moves around it.
    //
    // Read it here rather than beside the node-kind constants where upstream declares
    // it (search.cpp:726): a leaf returns at the dive above before either reader, and
    // this is two loads through ctx.
    const int32_t pv_line_score = ctx->root_moves[ctx->pv_idx].score;
    const bool seek_mate =
      ctx->root_depth >= 16 && (pv_line_score < 0 ? -pv_line_score : pv_line_score) >= 2000;

    Histories *const h = ctx->hist;
    Stack *const ss1 = ss - 1;
    Stack *const ss2 = ss - 2;

    Value alpha = alpha_in;
    Value beta = beta_in;
    int depth = depth_in < MAX_PLY - 1 ? depth_in : MAX_PLY - 1;

    // Detect the upcoming-repetition draw (non-root).
    if (!root_node && alpha < VALUE_DRAW && pos_upcoming_repetition(pos, ss->ply)) {
        alpha = search_value_draw(ctx_nodes(ctx));
        if (alpha >= beta)
            return alpha;
    }

    PVMoves pv;
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
            return search_value_draw(ctx_nodes(ctx));
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
    if (prior_reduction >= 2 && depth >= 2 && ss->static_eval + ss1->static_eval > 166)
        depth -= 1;

    // Step 6. Cut off early on the TT (non-PV).
    if (!pv_node && excluded_move == MOVE_NONE && tt_depth > depth - (int) (tt_value <= beta)
        && value_is_valid(tt_value)
        && (tt_bound_v & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != 0
        && (cut_node == (tt_value >= beta) || depth > 4)) {
        if (tt_move != MOVE_NONE && tt_value >= beta) {
            if (!tt_capture)  // upstream 73826352d
                search_update_quiet_histories(ctx, pos, ss, tt_move,
                                              112 * depth < 695 ? 112 * depth : 695);
            if (prev_sq != (int) SQ_NONE && ss1->move_count < 5 && !prior_capture)
                search_update_continuation_histories(ss1, piece_on(pos, (Square) prev_sq),
                                                     (Square) prev_sq, -2210);
        }
        if (pos->st->rule50 < 96) {
            if (depth >= 7 && tt_move != MOVE_NONE && search_pseudo_legal(pos, tt_move)
                && pos_legal(pos, tt_move) && !value_is_decisive(tt_value)) {
                pos_do_move(pos, tt_move, &st, search_gives_check(pos, tt_move), &pos->scratch_dp,
                            &pos->scratch_dts, nullptr);
                const Key next_key = adjust_key50(pos);
                const TTProbe probe_next = search_tt_probe(next_key);
                pos_undo_move(pos, tt_move);
                // Read the entry's value WITHOUT gating on `found`, as upstream
                // does (search.cpp:882-887). `found` is the occupancy test
                // (depth8 != 0), not a key test: a probe that matched key16 on a
                // penalised entry whose depth walked down to zero still carries a
                // real value16. Substituting VALUE_NONE there takes the cutoff in
                // a case upstream declines, and changes what gets stored below.
                const Value next_value = probe_next.value;
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

    // Step 7. Probe the tablebases. Probe the WDL of the current (non-root,
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

                // Force a check of time on the next occasion (search.cpp:917-919).
                // `stop_write` is null on a non-main thread, which is upstream's
                // is_mainthread() guard.
                if (ctx->time_state.stop_write)
                    ctx->time_state.calls_cnt = 0;
                if (res.available != 0) {
                    ctx_add_tb_hits(ctx, 1);
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
              diff * 11, HIST_LIMIT_MAIN);
            if (!tt_hit && type_of_piece(piece_on(pos, (Square) prev_sq)) != PAWN
                && move_type(ss1->current_move) != PROMOTION) {
                const Square psq = (Square) prev_sq;
                SharedStat *const row = pawn_history_row(h, pos->st->pawn_key);
                shared_stats_update(&row[(size_t) piece_on(pos, psq) * SQUARE_NB + (size_t) psq],
                                    diff * 13, HIST_LIMIT_PAWN);
            }
        }

        // Step 8. Apply razoring.
        if (!pv_node && eval < alpha - razor_margin(depth))
            return qsearch_node_nonpv(ctx, pos, ss, alpha, beta);

        // Step 9. Prune by futility.
        // The depth condition is important for mate finding. It shouldn't be tuned.
        if (!ss->tt_pv && depth < (seek_mate ? 6 : 19) && eval >= beta
            && (tt_move == MOVE_NONE || tt_capture) && !value_is_loss(beta)
            && !value_is_win(eval)) {
            const int fm =
              futility_margin(depth, ss->tt_hit, improving, opponent_worsening, correction_value);
            if (eval - fm >= beta)
                return (Value) futility_return(beta, eval);
        }

        // Step 10. Search the null move.
        if (cut_node && ss->static_eval >= null_move_threshold(beta, depth, improving)
            && excluded_move == MOVE_NONE && pos_non_pawn_material(pos, us) != 0
            && ss->ply >= ctx->nmp_min_ply && beta >= -2000) {
            const int r = null_move_reduction(depth, ss->static_eval, beta);
            // Touch no accumulator for a null move: mark the stack move as null and
            // install the all-NO_PIECE continuation pages.
            pos_do_null_move(pos, &st, &pos->scratch_dp, &pos->scratch_dts);
            ss->current_move = MOVE_NULL;
            search_set_cont_hist(ctx, ss, false, false, NO_PIECE, SQ_A1);
            const Value null_value =
              (Value) -search_node_nonpv(ctx, pos, ss + 1, -beta, -beta + 1, depth - r, false);
            pos_undo_null_move(pos);
            if (null_value >= beta && !value_is_win(null_value)) {
                if (ctx->nmp_min_ply != 0 || depth < 16)
                    return null_value;
                ctx->nmp_min_ply = nmp_min_ply_of(ss->ply, depth, r);
                const Value v = search_node_nonpv(ctx, pos, ss, beta - 1, beta, depth - r, false);
                ctx->nmp_min_ply = 0;
                if (v >= beta)
                    return null_value;
            }
        }

        if (ss->static_eval >= beta)
            improving = true;

        // Step 11. Apply internal iterative reductions.
        // upstream b1053e60b: drop the priorReduction <= 3 term.
        if (!ss->follow_pv && !all_node && depth >= 6 && tt_move == MOVE_NONE)
            depth -= 1;

        // Step 12. Run ProbCut.
        const int pc_beta = probcut_beta(beta, improving);
        if (depth >= 3 && !value_is_decisive(beta)
            && !(value_is_valid(tt_value) && tt_value < pc_beta)) {
            MovePicker mp;
            movepick_init_probcut(&mp, pos, h, tt_move, pc_beta - ss->static_eval);
            mp_set_probcut_stage(&mp, pos, tt_move);

            // Upstream ebcea3efe restructured this from `depth - 4 - improving`: when NOT
            // improving it is now depth - 3, not depth - 4. A number swap would miss that.
            const int probcut_depth = depth - (improving ? 5 : 3);
            Move move;
            while ((move = movepick_next(&mp)) != MOVE_NONE) {
                if (move == excluded_move || !pos_legal(pos, move))
                    continue;
                search_do_move(ctx, pos, move, &st, search_gives_check(pos, move), ss);
                Value value = (Value) -qsearch_node_nonpv(ctx, pos, ss + 1, -pc_beta, -pc_beta + 1);
                if (value >= pc_beta && probcut_depth > 0)
                    value = (Value) -search_node_nonpv(ctx, pos, ss + 1, -pc_beta, -pc_beta + 1,
                                                       probcut_depth, !cut_node);
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
    // Step 13. Apply the deep-ProbCut TT idea.
    const int pc_beta_deep = probcut_beta_deep(beta);
    if ((tt_bound_v & BOUND_LOWER) != 0 && tt_depth >= depth - 4 && tt_value >= pc_beta_deep
        && !value_is_decisive(beta) && value_is_valid(tt_value) && !value_is_decisive(tt_value))
        return (Value) pc_beta_deep;

    Move best_move = MOVE_NONE;

    // Gather the six continuation pages the move loop and the picker score
    // through, before the loop, as upstream does (search.cpp:1093).
    const SharedStat *const cont_hist[6] = {
        (ss - 1)->continuation_history, (ss - 2)->continuation_history,
        (ss - 3)->continuation_history, (ss - 4)->continuation_history,
        (ss - 5)->continuation_history, (ss - 6)->continuation_history,
    };

    MovePicker mp;
    movepick_init(&mp, pos, h, pos->st->pawn_key, tt_move, depth, ss->ply, cont_hist);
    mp_set_main_stage(&mp, pos, tt_move, depth);

    Value value = best_value;
    int move_count = 0;

    // The window term of the reduction formula, carried ACROSS the move loop.
    //
    // It is the one runtime divisor in reduction_of: root_delta is fixed for the whole
    // search and beta never moves inside this function, so the quotient can change only
    // where alpha is raised -- the single assignment in step 20. Computed per move
    // instead, it was a hardware integer divide on every move the node searches, and at
    // a non-PV node alpha is pinned to beta - 1 and cannot be raised at all, so there
    // the divide ran once per move to produce the same constant every time.
    int delta_scaled = (beta - alpha) * 577 / ctx->root_delta;
    Move quiets_searched[32];
    size_t n_quiets = 0;
    Move captures_searched[32];
    size_t n_captures = 0;

    // Step 14. Loop over moves.
    Move move;
    while ((move = movepick_next(&mp)) != MOVE_NONE) {
        if (move == excluded_move)
            continue;
        if (!pos_legal(pos, move))
            continue;
        if (root_node && !root_in_list(ctx, move))
            continue;

        move_count += 1;
        ss->move_count = move_count;

        if (root_node && ctx->is_main && ctx_nodes(ctx) > ID_NODES_LIMIT_OUTPUT)
            // Report moveCount + pvIdx, not moveCount: with MultiPV the number a GUI
            // shows is the move's index across all PV lines (search.cpp:1126). Only the
            // main thread emits it (search.cpp:1125), else every worker double-prints.
            search_emit_root_on_iter(ctx, depth, move, move_count + (int) ctx->pv_idx);

        if (pv_node)
            (ss + 1)->pv = nullptr;

        int extension = 0;
        const bool capture = search_capture_stage(pos, move);
        const Piece moved_piece = piece_on(pos, move_from(move));
        const Square to = move_to(move);
        const bool gc = search_gives_check(pos, move);

        int new_depth = depth - 1;
        int r = reduction_of(ctx->reductions, depth, move_count, delta_scaled, improving);
        if (ss->tt_pv)
            r += 929;

        // Step 15. Prune at shallow depth.
        if (!root_node && pos_non_pawn_material(pos, us) != 0 && !value_is_loss(best_value)) {
            if (move_count >= move_count_limit(depth, improving))
                movepick_skip_quiets(&mp);
            int lmr_depth = new_depth - r / 1024;
            if (capture || gc) {
                const Piece captured = piece_on(pos, to);
                const int capt_hist = *capture_entry(h, moved_piece, to, type_of_piece(captured));
                if (!gc && lmr_depth < 8) {
                    const int fv = capture_futility_value(ss->static_eval, lmr_depth,
                                                          PieceValueByPiece[captured], capt_hist);
                    if (fv <= alpha)
                        continue;
                }
                const int margin = capture_see_margin(depth, capt_hist);
                if ((alpha >= VALUE_DRAW
                     || pos_non_pawn_material(pos, us) != PieceValueByPiece[moved_piece])
                    && !see_ge(pos, move, -margin))
                    continue;
            } else if (!ss->follow_pv || !pv_node) {
                const int capped = depth < 16 ? depth : 16;
                const size_t d_index = (size_t) (capped - 1);
                int history =
                  cont_val(cont_hist[0], moved_piece, to) + cont_val(cont_hist[1], moved_piece, to)
                  + shared_stat_load(&pawn_history_row(
                    h, pos->st->pawn_key)[(size_t) moved_piece * SQUARE_NB + (size_t) to]);
                if (history < history_prune_threshold(depth))
                    continue;
                history +=
                  69 * (int) h->main_history[(size_t) us * HIST_UINT16 + (size_t) move] / 32;
                lmr_depth += history / LmrDivisor[d_index];
                const int fv =
                  quiet_futility_value(ss->static_eval, lmr_depth, ss->static_eval > alpha);
                if (!ss->in_check && lmr_depth < 12 && fv <= alpha) {
                    if (best_value <= fv && !value_is_decisive(best_value) && !value_is_win(fv))
                        best_value = (Value) fv;
                    continue;
                }
                if (lmr_depth < 0)
                    lmr_depth = 0;
                if (!see_ge(pos, move, -quiet_see_margin(lmr_depth)))
                    continue;
            }
        }

        // Step 16. Extend (singular).
        if (!root_node && move == tt_move && excluded_move == MOVE_NONE
            && depth >= 6 + (int) ss->tt_pv && value_is_valid(tt_value)
            && !value_is_decisive(tt_value) && (tt_bound_v & BOUND_LOWER) != 0
            && tt_depth >= depth - 3 && !is_shuffling(pos, ss, move) && !seek_mate) {
            const int sb = singular_beta(tt_value, ss->tt_pv && !pv_node, depth);
            const int singular_depth = new_depth / 2;
            ss->excluded_move = move;
            value = search_node_nonpv(ctx, pos, ss, (Value) (sb - 1), (Value) sb, singular_depth,
                                      cut_node);
            ss->excluded_move = MOVE_NONE;
            if (value < sb) {
                const bool ply_gt_root = ss->ply > ctx->root_depth;
                const int double_margin = singular_double_margin(
                  pv_node, !tt_capture, correction_value, h->tt_move_history, ply_gt_root);
                const int triple_margin = singular_triple_margin(pv_node, !tt_capture, ss->tt_pv,
                                                                 correction_value, ply_gt_root);
                extension =
                  1 + (int) (value < sb - double_margin) + (int) (value < sb - triple_margin);
                depth += 1;
            } else if (value >= beta && !value_is_decisive(value)) {
                tt_move_history_update(h, tt_move_history_depth_bonus(depth));

                if (!ss->in_check && value > ss->static_eval) {
                    history_update_correction(
                      h, pos, pos->side_to_move, ss1->current_move,
                      (ss - 2)->continuation_correction_history,
                      (ss - 4)->continuation_correction_history,
                      multicut_correction_bonus(value - ss->static_eval, singular_depth));
                }

                return value;
            } else if (tt_value >= beta || cut_node) {
                extension = -3;
            }
        }

        const uint64_t node_count = root_node ? ctx_nodes(ctx) : 0;

        // Step 17. Make the move.
        search_do_move(ctx, pos, move, &st, gc, ss);
        new_depth += extension;

        if (ss->tt_pv)
            r -= lmr_ttpv_reduction(pv_node, tt_value > alpha, tt_depth >= depth, cut_node);
        r += 697;
        r -= move_count * 65;
        r -= lmr_corr_reduction(correction_value);
        if (cut_node)
            r += 4026 + 933 * (int) (tt_move == MOVE_NONE);
        if ((ss + 1)->cutoff_cnt > 1) {
            r += 264 + 1095 * (int) ((ss + 1)->cutoff_cnt > 2) + 1138 * (int) all_node;
        } else if (move == tt_move) {
            // upstream 924d29d3c: simplify the first-picked-move (ttMove) reduction.
            r -= 2179;
        }
        if (tt_capture)
            r += 1079;

        if (capture) {
            const Piece cap_pc = captured_piece(pos);
            ss->stat_score = capture_stat_score(
              PieceValueByPiece[cap_pc], *capture_entry(h, moved_piece, to, type_of_piece(cap_pc)));
        } else {
            ss->stat_score = quiet_stat_score(
              h->main_history[(size_t) us * HIST_UINT16 + (size_t) move],
              cont_val(cont_hist[0], moved_piece, to), cont_val(cont_hist[1], moved_piece, to));
        }

        r -= lmr_stat_score_reduction(ss->stat_score);

        // Skip a decisive alpha: the gap is a mate distance there, not an
        // evaluation difference, so scaling a reduction by it means nothing.
        if (!capture && !value_is_decisive(alpha))
            r += lmr_alpha_gap_reduction(alpha, eval);

        if (all_node)
            r += lmr_all_node_scale(r, depth);

        // Step 18. Compute and apply the late-move reduction (or extension).
        // Steps 19 and 20 are the two branches below it: the full-depth search
        // when LMR is skipped, and the PV window on the first move or a fail-high.
        if (depth >= 2 && move_count > 1) {
            const int reduced = new_depth - r / 1024;
            const int capped = reduced < new_depth + 2 ? reduced : new_depth + 2;
            const int d = (capped > 1 ? capped : 1) + (int) pv_node;
            ss->reduction = new_depth - d;
            value =
              (Value) -search_node_nonpv(ctx, pos, ss + 1, (Value) - (alpha + 1), -alpha, d, true);
            ss->reduction = 0;
            if (value > alpha) {
                const bool do_deeper = d < new_depth && value > best_value + 53;
                const bool do_shallower = value < best_value + 8;
                new_depth += (int) do_deeper - (int) do_shallower;
                if (new_depth > d)
                    value = (Value) -search_node_nonpv(ctx, pos, ss + 1, (Value) - (alpha + 1),
                                                       -alpha, new_depth, !cut_node);
                search_update_continuation_histories(ss, moved_piece, to, 1334);
            }
        } else if (!pv_node || move_count > 1) {
            if (tt_move == MOVE_NONE)
                r += 1127;
            const int d = new_depth - (int) (r > 5234) - (int) (r > 5487 && new_depth > 2);
            value = (Value) -search_node_nonpv(ctx, pos, ss + 1, (Value) - (alpha + 1), -alpha, d,
                                               !cut_node);
        }

        if (pv_node && (move_count == 1 || value > alpha)) {
            (ss + 1)->pv = &pv;
            pv_clear(&pv);
            if (move == tt_move
                && ((value_is_valid(tt_value) && value_is_decisive(tt_value) && tt_depth > 0)
                    || tt_depth > 1))
                new_depth = new_depth > 1 ? new_depth : 1;
            value = (Value) -search_node_pv(ctx, pos, ss + 1, -beta, -alpha, new_depth, false);
        }

        // Step 21. Undo move.
        search_undo_move(ctx, pos, move);

        // Step 22. Check for a new best move.
        if (search_stopped(ctx))
            return VALUE_DRAW;

        if (root_node) {
            // (ss + 1)->pv is only valid (non-null) when this move ran a PV search,
            // i.e. move_count == 1 or value > alpha; otherwise it is ignored.
            const PVMoves *const cpv = (move_count == 1 || value > alpha) ? (ss + 1)->pv : nullptr;
            root_update(ctx, move, value, ctx_nodes(ctx) - node_count, move_count, alpha, beta,
                        cpv);
        }

        const Value av = value < 0 ? -value : value;
        const int inc = (int) (value == best_value && ss->ply + 2 >= ctx->root_depth
                               && (int) (ctx_nodes(ctx) & 14) == 0 && !value_is_win(av + 1));
        if (value + inc > best_value) {
            best_value = value;
            if (value + inc > alpha) {
                best_move = move;
                // (ss + 1)->pv is set only when this move ran a PV re-search; if a
                // rare best-move update fires without one it stays null, and
                // pv_update takes the child PV as optional (null -> the PV is just
                // the move).
                if (pv_node && !root_node)
                    pv_update(ss->pv, move, (ss + 1)->pv);
                if (value >= beta) {
                    ss->cutoff_cnt += (int) (extension < 2 || pv_node);
                    break;
                }
                if (depth > 3 && depth < 12 && !value_is_decisive(value))
                    depth -= 3;
                alpha = value;
                delta_scaled = (beta - alpha) * 577 / ctx->root_delta;
            }
        }

        if (move != best_move && move_count <= 32) {
            if (capture)
                captures_searched[n_captures++] = move;
            else
                quiets_searched[n_quiets++] = move;
        }
    }

    // Step 23. Adjust for mate / stalemate / fail-high.
    if (best_value >= beta && !value_is_decisive(best_value) && !value_is_decisive(alpha))
        best_value = (Value) ((best_value * depth + beta) / (depth + 1));

    if (move_count == 0) {
        best_value = excluded_move != MOVE_NONE ? alpha
                   : ss->in_check               ? mated_in(ss->ply)
                                                : VALUE_DRAW;
    } else if (best_move != MOVE_NONE) {
        search_update_all_stats(ctx, pos, ss, best_move, (Square) prev_sq, quiets_searched,
                                n_quiets, captures_searched, n_captures, depth, tt_move, pv_node);
        if (!pv_node)
            tt_move_history_update(h, tt_move_history_match_bonus(best_move == tt_move));
    } else if (!prior_capture && prev_sq != (int) SQ_NONE) {
        const Square psq = (Square) prev_sq;
        const int bonus_scale =
          prior_bonus_scale(ss1->stat_score, depth, ss1->move_count > 9,
                            !ss->in_check && best_value <= ss->static_eval - 106,
                            !ss1->in_check && best_value <= -ss1->static_eval - 68);
        const int scaled_bonus = prior_scaled_bonus_base(depth) * bonus_scale;
        const Piece prev_pc = piece_on(pos, psq);

        search_update_continuation_histories(ss1, prev_pc, psq, prior_conthist_scale(scaled_bonus));
        stats_update(
          &h->main_history[(size_t) flip_color(us) * HIST_UINT16 + (size_t) ss1->current_move],
          prior_mainhist_scale(scaled_bonus), HIST_LIMIT_MAIN);
        if (type_of_piece(prev_pc) != PAWN && move_type(ss1->current_move) != PROMOTION) {
            SharedStat *const row = pawn_history_row(h, pos->st->pawn_key);
            shared_stats_update(&row[(size_t) prev_pc * SQUARE_NB + (size_t) psq],
                                prior_pawnhist_scale(scaled_bonus), HIST_LIMIT_PAWN);
        }
    } else if (prior_capture && prev_sq != (int) SQ_NONE) {
        const Square psq = (Square) prev_sq;
        stats_update(capture_entry(h, piece_on(pos, psq), psq, type_of_piece(captured_piece(pos))),
                     892, HIST_LIMIT_CAPTURE);
    }

    if (pv_node)
        best_value = best_value < max_value ? best_value : max_value;

    if (best_value <= alpha)
        ss->tt_pv = ss->tt_pv || ss1->tt_pv;

    // Step 24. Write the gathered information to the transposition table. The
    // static evaluation is stored as it was BEFORE correction history.
    if (excluded_move == MOVE_NONE && !(root_node && ctx->pv_idx != 0)) {
        const Bound bound = best_value >= beta                  ? BOUND_LOWER
                          : (pv_node && best_move != MOVE_NONE) ? BOUND_EXACT
                                                                : BOUND_UPPER;
        const int wdepth =
          move_count != 0 ? depth : (depth + 6 < MAX_PLY - 1 ? depth + 6 : MAX_PLY - 1);
        search_tt_save(writer, pos_key, search_value_to_tt(best_value, ss->ply), ss->tt_pv, bound,
                       wdepth, best_move, unadjusted_static_eval);
    }

    // Adjust the correction history. Pass the three stack facts the update reads
    // directly.
    if (!ss->in_check && !(best_move != MOVE_NONE && pos_capture(pos, best_move))
        && (best_value > ss->static_eval) == (best_move != MOVE_NONE)) {
        history_update_correction(
          h, pos, pos->side_to_move, ss1->current_move, (ss - 2)->continuation_correction_history,
          (ss - 4)->continuation_correction_history,
          correction_history_bonus(best_value - ss->static_eval, depth, best_move != MOVE_NONE));
    }

    return best_value;
}

// Emit the three specializations upstream's template produces. Each is one
// inlined copy of the body with its NodeType folded. The NonPV and PV clones are
// exported (search_main.h) so the move loop's literal-NT recursion calls them
// directly, as upstream's `search<NonPV>` call sites do; the tag dispatcher below
// serves only the callers whose NodeType is not a compile-time literal.
Value search_node_nonpv(
  SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, int depth, bool cut_node) {
    return search_node_impl(ctx, pos, ss, alpha, beta, depth, cut_node, NT_NON_PV);
}

Value search_node_pv(
  SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, int depth, bool cut_node) {
    return search_node_impl(ctx, pos, ss, alpha, beta, depth, cut_node, NT_PV);
}

static Value search_node_root(
  SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, int depth, bool cut_node) {
    return search_node_impl(ctx, pos, ss, alpha, beta, depth, cut_node, NT_ROOT);
}

Value search_node(SearchCtx *ctx,
                  Position *pos,
                  Stack *ss,
                  Value alpha_in,
                  Value beta_in,
                  int depth_in,
                  bool cut_node,
                  NodeType nt) {
    if (nt == NT_NON_PV)
        return search_node_nonpv(ctx, pos, ss, alpha_in, beta_in, depth_in, cut_node);
    if (nt == NT_PV)
        return search_node_pv(ctx, pos, ss, alpha_in, beta_in, depth_in, cut_node);
    return search_node_root(ctx, pos, ss, alpha_in, beta_in, depth_in, cut_node);
}
