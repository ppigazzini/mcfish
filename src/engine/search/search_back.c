#include "search_back.h"

#include "movepick.h"
#include "search_common.h"
#include "search_control.h"
#include "search_emit.h"
#include "search_main.h"
#include "search_qsearch.h"

#include "../board/score.h"

// Emit the "currmove" line only once the search is long enough for it to be
// worth reading (upstream's 10M-node threshold).
enum : uint64_t { ID_NODES_LIMIT_OUTPUT = 10000000 };

// Collect the six continuation pages (ss-1)..(ss-6) the picker scores from.
static void collect_cont_hist(const Stack *ss, const int16_t *cont[6]) {
    for (size_t k = 0; k < 6; ++k)
        cont[k] = (ss - 1 - (ptrdiff_t) k)->continuation_history;
}

// Enter the picker at the TT stage only when the TT move is usable, but keep
// `tt_move` set either way so the generated list still filters it out.
static void mp_set_main_stage(MovePicker *mp, const Position *pos, Move tt_move, int depth) {
    const int base = checkers(pos) != 0 ? MP_EVASION_TT : depth > 0 ? MP_MAIN_TT : MP_QSEARCH_TT;
    const bool usable = tt_move != MOVE_NONE && search_pseudo_legal(pos, tt_move);
    mp->stage = base + (int) (!usable);
}

Value search_run_back(const SearchNodeState *nd) {
    SearchCtx *const ctx = nd->ctx;
    Position *const pos = nd->pos;
    Stack *const ss = nd->ss;
    Stack *const ss1 = nd->ss1;
    Histories *const h = nd->hist;

    Value alpha = nd->alpha;
    int depth = nd->depth;
    Value best_value = nd->best_value;
    Move best_move = MOVE_NONE;
    StateInfo st;
    PVMoves pv;

    const int16_t *cont_hist[6];
    collect_cont_hist(ss, cont_hist);

    MovePicker mp;
    movepick_init(&mp, pos, h, pos->st->pawn_key, nd->tt_move, depth, ss->ply, cont_hist);
    mp_set_main_stage(&mp, pos, nd->tt_move, depth);

    Value value = best_value;
    int move_count = 0;
    Move quiets_searched[32];
    size_t n_quiets = 0;
    Move captures_searched[32];
    size_t n_captures = 0;

    // Step 13. Loop over moves.
    for (Move move = movepick_next(&mp); move != MOVE_NONE; move = movepick_next(&mp)) {
        if (move == nd->excluded_move)
            continue;
        if (!pos_legal(pos, move))
            continue;
        if (nd->root_node && !root_in_list(ctx, move))
            continue;

        move_count += 1;
        ss->move_count = move_count;

        if (nd->root_node && ctx->nodes > ID_NODES_LIMIT_OUTPUT)
            // Report moveCount + pvIdx, not moveCount: with MultiPV the number a GUI
            // shows is the move's index across all PV lines (search.cpp:1126).
            search_emit_root_on_iter(ctx, depth, move, move_count + (int) ctx->pv_idx);

        if (nd->pv_node)
            (ss + 1)->pv = nullptr;

        int extension = 0;
        const bool capture = search_capture_stage(pos, move);
        const Piece moved_piece = piece_on(pos, move_from(move));
        const Square to = move_to(move);
        const bool gc = search_gives_check(pos, move);

        int new_depth = depth - 1;
        const int delta = nd->beta - alpha;
        int r =
          reduction_of(ctx->reductions, depth, move_count, delta, ctx->root_delta, nd->improving);
        if (ss->tt_pv)
            r += 1006;

        // Step 14. Prune at shallow depth.
        if (!nd->root_node && pos_non_pawn_material(pos, nd->us) != 0
            && !value_is_loss(best_value)) {
            if (move_count >= move_count_limit(depth, nd->improving))
                movepick_skip_quiets(&mp);
            int lmr_depth = new_depth - r / 1024;
            if (capture || gc) {
                const Piece captured = piece_on(pos, to);
                const int capt_hist = *capture_entry(h, moved_piece, to, type_of_piece(captured));
                if (!gc && lmr_depth < 7) {
                    const int fv = capture_futility_value(ss->static_eval, lmr_depth,
                                                          PieceValueByPiece[captured], capt_hist);
                    if (fv <= alpha)
                        continue;
                }
                const int margin = capture_see_margin(depth, capt_hist);
                if ((alpha >= VALUE_DRAW
                     || pos_non_pawn_material(pos, nd->us) != PieceValueByPiece[moved_piece])
                    && !see_ge(pos, move, -margin))
                    continue;
            } else if (!ss->follow_pv || !nd->pv_node) {
                const int capped = depth < 16 ? depth : 16;
                const size_t d_index = (size_t) (capped - 1);
                int history =
                  cont_val(cont_hist[0], moved_piece, to) + cont_val(cont_hist[1], moved_piece, to)
                  + pawn_history_row(
                    h, pos->st->pawn_key)[(size_t) moved_piece * SQUARE_NB + (size_t) to];
                if (history < history_prune_threshold(depth))
                    continue;
                history +=
                  64 * (int) h->main_history[(size_t) nd->us * HIST_UINT16 + (size_t) move] / 32;
                lmr_depth += history / LmrDivisor[d_index];
                const int fv = quiet_futility_value(ss->static_eval, best_move == MOVE_NONE,
                                                    lmr_depth, ss->static_eval > alpha);
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

        // Step 15. Extend (singular).
        if (!nd->root_node && move == nd->tt_move && nd->excluded_move == MOVE_NONE
            && depth >= 6 + (int) ss->tt_pv && value_is_valid(nd->tt_value)
            && !value_is_decisive(nd->tt_value) && (nd->tt_bound & BOUND_LOWER) != 0
            && nd->tt_depth >= depth - 3 && !is_shuffling(pos, ss, move)) {
            const int sb = singular_beta(nd->tt_value, ss->tt_pv && !nd->pv_node, depth);
            const int singular_depth = new_depth / 2;
            ss->excluded_move = move;
            value = search_node(ctx, pos, ss, (Value) (sb - 1), (Value) sb, singular_depth,
                                nd->cut_node, NT_NON_PV);
            ss->excluded_move = MOVE_NONE;
            if (value < sb) {
                const bool ply_gt_root = ss->ply > ctx->root_depth;
                const int double_margin =
                  singular_double_margin(nd->pv_node, !nd->tt_capture, nd->correction_value,
                                         h->tt_move_history, ply_gt_root);
                const int triple_margin = singular_triple_margin(
                  nd->pv_node, !nd->tt_capture, ss->tt_pv, nd->correction_value, ply_gt_root);
                extension =
                  1 + (int) (value < sb - double_margin) + (int) (value < sb - triple_margin);
                depth += 1;
            } else if (value >= nd->beta && !value_is_decisive(value)) {
                tt_move_history_update(h, tt_move_history_depth_bonus(depth));
                return value;
            } else if (nd->tt_value >= nd->beta) {
                extension = -3;
            } else if (nd->cut_node) {
                extension = -2;
            }
        }

        const uint64_t node_count = nd->root_node ? ctx->nodes : 0;

        // Step 16. Make the move.
        search_do_move(ctx, pos, move, &st, gc, ss);
        new_depth += extension;

        if (ss->tt_pv)
            r -= lmr_ttpv_reduction(nd->pv_node, nd->tt_value > alpha, nd->tt_depth >= depth,
                                    nd->cut_node);
        r += 714;
        r -= move_count * 62;
        r -= lmr_corr_reduction(nd->correction_value);
        if (nd->cut_node)
            r += 3995 + 1059 * (int) (nd->tt_move == MOVE_NONE);
        if ((ss + 1)->cutoff_cnt > 1) {
            r += 236 + 1079 * (int) ((ss + 1)->cutoff_cnt > 2) + 1143 * (int) nd->all_node;
        } else if (move == nd->tt_move) {
            // upstream 924d29d3c: simplify the first-picked-move (ttMove) reduction.
            r -= 2016;
        }
        if (nd->tt_capture)
            r += 1039;

        if (capture) {
            const Piece cap_pc = captured_piece(pos);
            ss->stat_score = capture_stat_score(
              PieceValueByPiece[cap_pc], *capture_entry(h, moved_piece, to, type_of_piece(cap_pc)));
        } else {
            ss->stat_score = quiet_stat_score(
              h->main_history[(size_t) nd->us * HIST_UINT16 + (size_t) move],
              cont_val(cont_hist[0], moved_piece, to), cont_val(cont_hist[1], moved_piece, to));
        }

        r -= lmr_stat_score_reduction(ss->stat_score);
        if (nd->all_node)
            r += lmr_all_node_scale(r, depth);

        // Step 17/18. Run the LMR + full-depth search.
        if (depth >= 2 && move_count > 1) {
            const int reduced = new_depth - r / 1024;
            const int capped = reduced < new_depth + 2 ? reduced : new_depth + 2;
            const int d = (capped > 1 ? capped : 1) + (int) nd->pv_node;
            ss->reduction = new_depth - d;
            value = (Value) -search_node(ctx, pos, ss + 1, (Value) - (alpha + 1), -alpha, d, true,
                                         NT_NON_PV);
            ss->reduction = 0;
            if (value > alpha) {
                const bool do_deeper = d < new_depth && value > best_value + 52;
                const bool do_shallower = value < best_value + 9;
                new_depth += (int) do_deeper - (int) do_shallower;
                if (new_depth > d)
                    value = (Value) -search_node(ctx, pos, ss + 1, (Value) - (alpha + 1), -alpha,
                                                 new_depth, !nd->cut_node, NT_NON_PV);
                search_update_continuation_histories(ss, moved_piece, to, 1415);
            }
        } else if (!nd->pv_node || move_count > 1) {
            if (nd->tt_move == MOVE_NONE)
                r += 1085;
            const int d = new_depth - (int) (r > 5039) - (int) (r > 5223 && new_depth > 2);
            value = (Value) -search_node(ctx, pos, ss + 1, (Value) - (alpha + 1), -alpha, d,
                                         !nd->cut_node, NT_NON_PV);
        }

        if (nd->pv_node && (move_count == 1 || value > alpha)) {
            (ss + 1)->pv = &pv;
            pv_clear(&pv);
            if (move == nd->tt_move
                && ((value_is_valid(nd->tt_value) && value_is_decisive(nd->tt_value)
                     && nd->tt_depth > 0)
                    || nd->tt_depth > 1))
                new_depth = new_depth > 1 ? new_depth : 1;
            value =
              (Value) -search_node(ctx, pos, ss + 1, -nd->beta, -alpha, new_depth, false, NT_PV);
        }

        // Step 19. Undo move.
        search_undo_move(ctx, pos, move);

        // Step 20. Check for a new best move.
        if (search_stopped(ctx))
            return VALUE_DRAW;

        if (nd->root_node) {
            // (ss + 1)->pv is only valid (non-null) when this move ran a PV search,
            // i.e. move_count == 1 or value > alpha; otherwise it is ignored.
            const PVMoves *const cpv = (move_count == 1 || value > alpha) ? (ss + 1)->pv : nullptr;
            root_update(ctx, move, value, ctx->nodes - node_count, move_count, alpha, nd->beta,
                        cpv);
        }

        const Value av = value < 0 ? -value : value;
        const int inc = (int) (value == best_value && ss->ply + 2 >= ctx->root_depth
                               && (int) (ctx->nodes & 14) == 0 && !value_is_win(av + 1));
        if (value + inc > best_value) {
            best_value = value;
            if (value + inc > alpha) {
                best_move = move;
                // (ss + 1)->pv is set only when this move ran a PV re-search; if a
                // rare best-move update fires without one it stays null, and
                // pv_update takes the child PV as optional (null -> the PV is just
                // the move).
                if (nd->pv_node && !nd->root_node)
                    pv_update(ss->pv, move, (ss + 1)->pv);
                if (value >= nd->beta) {
                    ss->cutoff_cnt += (int) (extension < 2 || nd->pv_node);
                    break;
                }
                if (depth > 2 && depth < 13 && !value_is_decisive(value))
                    depth -= 2;
                alpha = value;
            }
        }

        if (move != best_move && move_count <= 32) {
            if (capture)
                captures_searched[n_captures++] = move;
            else
                quiets_searched[n_quiets++] = move;
        }
    }

    // Step 21. Adjust for mate / stalemate / fail-high.
    if (best_value >= nd->beta && !value_is_decisive(best_value) && !value_is_decisive(alpha))
        best_value = (Value) ((best_value * depth + nd->beta) / (depth + 1));

    if (move_count == 0) {
        best_value = nd->excluded_move != MOVE_NONE ? alpha
                   : ss->in_check                   ? mated_in(ss->ply)
                                                    : VALUE_DRAW;
    } else if (best_move != MOVE_NONE) {
        const HistoryStack hs = search_gather_stack(ss);
        const HistoryStats stats = {
            .best_move = best_move,
            .prev_sq = (Square) nd->prev_sq,
            .quiets = quiets_searched,
            .n_quiets = n_quiets,
            .captures = captures_searched,
            .n_captures = n_captures,
            .depth = depth,
            .tt_move = nd->tt_move,
            .pv_node = nd->pv_node,
        };
        history_update_all_stats(h, pos, pos->st->pawn_key, &hs, &stats);
        if (!nd->pv_node)
            tt_move_history_update(h, tt_move_history_match_bonus(best_move == nd->tt_move));
    } else if (!nd->prior_capture && nd->prev_sq != (int) SQ_NONE) {
        const Square psq = (Square) nd->prev_sq;
        const int bonus_scale =
          prior_bonus_scale(ss1->stat_score, depth, ss1->move_count > 8,
                            !ss->in_check && best_value <= ss->static_eval - 103,
                            !ss1->in_check && best_value <= -ss1->static_eval - 78);
        const int scaled_bonus = prior_scaled_bonus_base(depth) * bonus_scale;
        const Piece prev_pc = piece_on(pos, psq);

        search_update_continuation_histories(ss1, prev_pc, psq, prior_conthist_scale(scaled_bonus));
        stats_update(
          &h->main_history[(size_t) flip_color(nd->us) * HIST_UINT16 + (size_t) ss1->current_move],
          prior_mainhist_scale(scaled_bonus), 7183);
        if (type_of_piece(prev_pc) != PAWN && move_type(ss1->current_move) != PROMOTION) {
            int16_t *const row = pawn_history_row(h, pos->st->pawn_key);
            stats_update(&row[(size_t) prev_pc * SQUARE_NB + (size_t) psq],
                         prior_pawnhist_scale(scaled_bonus), 8192);
        }
    } else if (nd->prior_capture && nd->prev_sq != (int) SQ_NONE) {
        const Square psq = (Square) nd->prev_sq;
        stats_update(capture_entry(h, piece_on(pos, psq), psq, type_of_piece(captured_piece(pos))),
                     901, 10692);
    }

    if (nd->pv_node)
        best_value = best_value < nd->max_value ? best_value : nd->max_value;

    if (best_value <= alpha)
        ss->tt_pv = ss->tt_pv || ss1->tt_pv;

    if (nd->excluded_move == MOVE_NONE && !(nd->root_node && ctx->pv_idx != 0)) {
        const Bound bound = best_value >= nd->beta                  ? BOUND_LOWER
                          : (nd->pv_node && best_move != MOVE_NONE) ? BOUND_EXACT
                                                                    : BOUND_UPPER;
        const int wdepth =
          move_count != 0 ? depth : (depth + 6 < MAX_PLY - 1 ? depth + 6 : MAX_PLY - 1);
        search_tt_save(nd->writer, nd->pos_key, search_value_to_tt(best_value, ss->ply), ss->tt_pv,
                       bound, wdepth, best_move, nd->unadjusted_static_eval);
    }

    // Adjust the correction history.
    if (!ss->in_check && !(best_move != MOVE_NONE && pos_capture(pos, best_move))
        && (best_value > ss->static_eval) == (best_move != MOVE_NONE)) {
        const HistoryStack hs = search_gather_stack(ss);
        const CorrectionKeys keys = {
            .pawn = pos->st->pawn_key,
            .minor = pos->st->minor_piece_key,
            .non_pawn = { pos->st->non_pawn_key[WHITE], pos->st->non_pawn_key[BLACK] },
        };
        history_update_correction(
          h, pos, pos->side_to_move, &keys, &hs,
          correction_history_bonus(best_value - ss->static_eval, depth, best_move != MOVE_NONE));
    }

    return best_value;
}
