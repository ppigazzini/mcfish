#include "search_id.h"

#include "search_common.h"
#include "search_emit.h"
#include "search_main.h"
#include "search_setup.h"
#include "time_source.h"

#include "../board/board_props.h"
#include "../board/score.h"

#include <stdatomic.h>
#include <string.h>

bool root_less(const RootMove *a, const RootMove *b) {
    return a->score != b->score ? a->score > b->score : a->previous_score > b->previous_score;
}

void stable_sort_root(RootMove *rm, size_t lo, size_t hi) {
    if (hi <= lo)
        return;
    for (size_t i = lo + 1; i < hi; ++i) {
        const RootMove key = rm[i];
        size_t j = i;
        while (j > lo && root_less(&key, &rm[j - 1])) {
            rm[j] = rm[j - 1];
            --j;
        }
        rm[j] = key;
    }
}

void move_to_front(RootMove *rm, size_t count, Move target) {
    size_t fi = 0;
    while (fi < count && rm[fi].pv.moves[0] != target)
        ++fi;
    if (fi >= count)
        return;
    const RootMove tmp = rm[fi];
    for (size_t z = fi; z > 0; --z)
        rm[z] = rm[z - 1];
    rm[0] = tmp;
}

static double fclamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static TimePoint id_elapsed(const SearchCtx *ctx, const SearchIdState *id) {
    return id->tm_use_nodes_time ? (TimePoint) ctx->nodes : TimeNowMs() - id->tm_start_time;
}

// Sum and reset the per-worker bestMoveChanges counter. One worker today, so the
// sum is that worker's; keep the shape so a pool is a loop, not a rewrite.
static double id_collect_bmc(SearchCtx *ctx) {
    const double total = (double) ctx->best_move_changes;
    ctx->best_move_changes = 0;
    return total;
}

// ---- skill handicap ----------------------------------------------------
//
// Match misc.h's xorshift* PRNG, seeded once from the clock on first use.
// Non-deterministic by design: the handicap is meant to vary between games.

enum : int32_t { SKILL_PAWN_VALUE = 208 };
static uint64_t SkillRngState = 0;

static uint64_t skill_rand64(void) {
    if (SkillRngState == 0)
        SkillRngState = (uint64_t) TimeNowMs();
    uint64_t s = SkillRngState;
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    SkillRngState = s;
    return s * 2685821657736338717ULL;
}

static bool skill_time_to_pick(double level, int depth) { return depth == 1 + (int) level; }

// Pick a sub-optimal move by a statistical rule over the descending-sorted root
// moves: weaker levels push a larger random bonus onto the lower-scored lines.
static Move skill_pick_best(const SearchCtx *ctx, const SearchIdState *id, size_t multi_pv) {
    const int32_t top_score = ctx->root_moves[0].score;
    const int32_t span = top_score - ctx->root_moves[multi_pv - 1].score;
    const int32_t delta = span < SKILL_PAWN_VALUE ? span : SKILL_PAWN_VALUE;
    const double weakness = 120.0 - 2.0 * id->skill_level;
    const uint32_t modw = (uint32_t) weakness;
    int32_t max_score = -VALUE_INFINITE;
    Move best = MOVE_NONE;

    for (size_t i = 0; i < multi_pv; ++i) {
        const uint32_t r = (uint32_t) skill_rand64();
        const double term1 = weakness * (double) (top_score - ctx->root_moves[i].score);
        const int32_t term2 = delta * (int32_t) (r % modw);
        const int32_t push = (int32_t) (term1 + (double) term2) / 128;
        if (ctx->root_moves[i].score + push >= max_score) {
            max_score = ctx->root_moves[i].score + push;
            best = ctx->root_moves[i].pv.moves[0];
        }
    }
    return best;
}

static void skill_swap_best(SearchCtx *ctx, Move move) {
    size_t i = 0;
    while (i < ctx->root_moves_count && ctx->root_moves[i].pv.moves[0] != move)
        ++i;
    if (i >= ctx->root_moves_count || i == 0)
        return;
    const RootMove tmp = ctx->root_moves[0];
    ctx->root_moves[0] = ctx->root_moves[i];
    ctx->root_moves[i] = tmp;
}

// ---- the depth loop ----------------------------------------------------

static bool id_stopped(const SearchCtx *ctx) {
    return atomic_load_explicit(ctx->stop, memory_order_relaxed);
}

bool iterative_deepening(SearchCtx *ctx, SearchIdState *id) {
    const bool main_thread = id->is_main;

    // Keep the stack automatic, not static: one array per search is what makes a
    // second worker a second call rather than a rewrite.
    PVMoves pv;
    Stack stack[STACK_SIZE];
    search_stack_init(stack, STACK_SIZE, ctx->hist, &pv);
    Stack *const ss = stack + STACK_PAD;

    // Keep the best move's PV in a local, as upstream does. It is the
    // abort-rollback memory and is written only when the best move changes — a
    // different quantity, and a different lifetime, from the per-pvIdx follow-PV
    // memory in ctx->last_iter_pv. Sharing one buffer conflates them.
    PVMoves last_best_move_pv;
    last_best_move_pv.length = 0;
    int32_t last_best_move_score = -VALUE_INFINITE;
    int32_t last_best_move_depth = 0;
    Value best_value = -VALUE_INFINITE;

    const Color us = board_side_to_move(ctx->root_pos);
    double time_reduction = 1.0;
    double tot_best_move_changes = 0.0;
    size_t iter_idx = 0;

    if (main_thread) {
        const int32_t fv = id->best_previous_score == VALUE_INFINITE ? 0 : id->best_previous_score;
        for (size_t k = 0; k < 4; ++k)
            id->iter_value[k] = fv;
    }

    size_t multi_pv = id->multipv_option;
    if (id->skill_enabled && multi_pv < 4)
        multi_pv = 4;
    if (multi_pv > ctx->root_moves_count)
        multi_pv = ctx->root_moves_count;
    Move skill_best = MOVE_NONE;

    history_fill_low_ply(ctx->hist);
    history_age_main(ctx->hist);

    int search_again_counter = 0;
    bool uci_pv_sent = false;

    while (ctx->root_depth + 1 < MAX_PLY && !id_stopped(ctx)
           && !(id->limits_depth != 0 && main_thread && ctx->root_depth >= id->limits_depth)) {
        ctx->root_depth += 1;

        if (main_thread) {
            tot_best_move_changes /= 2.0;
            uci_pv_sent = false;
        }

        // Save the last iteration's scores before the first PV line is searched
        // and every non-PV move's score is reset to -VALUE_INFINITE. The PV and
        // its exactness are saved alongside the score: the follow-PV heuristic
        // needs THIS line's previous PV, which rootMoves[0].pv cannot supply once
        // MultiPV > 1.
        for (size_t ri = 0; ri < ctx->root_moves_count; ++ri) {
            ctx->root_moves[ri].previous_score = ctx->root_moves[ri].score;
            ctx->root_moves[ri].previous_pv = ctx->root_moves[ri].pv;
            ctx->root_moves[ri].previous_score_exact = ri < multi_pv;
        }

        size_t pv_first = 0;
        ctx->pv_last = 0;

        if (!atomic_load_explicit(id->increase_depth, memory_order_relaxed))
            search_again_counter += 1;

        // Loop over the MultiPV lines.
        for (ctx->pv_idx = 0; ctx->pv_idx < multi_pv; ++ctx->pv_idx) {
            if (ctx->pv_idx == ctx->pv_last) {
                pv_first = ctx->pv_last;
                ctx->pv_last += 1;
                for (; ctx->pv_last < ctx->root_moves_count; ++ctx->pv_last)
                    if (ctx->root_moves[ctx->pv_last].tb_rank != ctx->root_moves[pv_first].tb_rank)
                        break;
            }

            // Point the follow-PV memory at THIS line's PV from the previous
            // iteration. It is per-pvIdx, NOT the best move's PV.
            ctx->last_iter_pv = ctx->root_moves[ctx->pv_idx].previous_pv;

            ctx->sel_depth = 0;

            int delta = aspiration_initial_delta(id->thread_idx,
                                                 ctx->root_moves[ctx->pv_idx].mean_squared_score);
            const int32_t avg = ctx->root_moves[ctx->pv_idx].average_score;
            Value alpha = (Value) (avg - delta > -VALUE_INFINITE ? avg - delta : -VALUE_INFINITE);
            Value beta = (Value) (avg + delta < VALUE_INFINITE ? avg + delta : VALUE_INFINITE);
            ctx->optimism[us] = optimism_of(avg);
            ctx->optimism[flip_color(us)] = -ctx->optimism[us];

            int failed_high_cnt = 0;
            for (;;) {
                const int raw_depth =
                  ctx->root_depth - failed_high_cnt - 3 * (search_again_counter + 1) / 4;
                const int adjusted_depth = raw_depth > 1 ? raw_depth : 1;
                ctx->root_delta = beta - alpha;
                best_value =
                  search_node(ctx, ctx->root_pos, ss, alpha, beta, adjusted_depth, false, NT_ROOT);

                stable_sort_root(ctx->root_moves, ctx->pv_idx, ctx->pv_last);

                if (id_stopped(ctx))
                    break;

                if (main_thread && multi_pv == 1 && (best_value <= alpha || best_value >= beta)
                    && ctx->nodes > ID_NODES_LIMIT_OUTPUT)
                    search_emit_pv(ctx, ctx->root_depth);

                if (best_value <= alpha) {
                    beta = alpha;
                    alpha = (Value) (best_value - delta > -VALUE_INFINITE ? best_value - delta
                                                                          : -VALUE_INFINITE);
                    failed_high_cnt = 0;
                    if (main_thread && id->stop_on_ponderhit)
                        *id->stop_on_ponderhit = false;
                } else if (best_value >= beta) {
                    alpha = (Value) (beta - delta > alpha ? beta - delta : alpha);
                    beta = (Value) (best_value + delta < VALUE_INFINITE ? best_value + delta
                                                                        : VALUE_INFINITE);
                    failed_high_cnt += 1;
                } else {
                    break;
                }

                delta = aspiration_delta_grow(delta);
            }

            // In MultiPV analysis, do not let an aborted line spoil a proven loss
            // from a completed earlier line, and do not trust an exact loss score
            // from an aborted search.
            if (id_stopped(ctx) && ctx->pv_idx != 0) {
                RootMove *const cur = &ctx->root_moves[ctx->pv_idx];
                RootMove *const prev_line = &ctx->root_moves[ctx->pv_idx - 1];
                const bool prev_is_loss = value_is_loss(prev_line->score);

                if ((prev_is_loss && root_less(cur, prev_line))
                    || root_move_score_is_exact_loss(cur, value_is_loss(cur->score))) {
                    // An exact previous score worse than pvIdx - 1 is safe to use;
                    // if it is equal, make sure it cannot overtake pvIdx - 1.
                    if (cur->previous_score != -VALUE_INFINITE && cur->previous_score_exact
                        && cur->previous_score <= prev_line->score) {
                        cur->score = cur->previous_score;
                        cur->uci_score = cur->previous_score;
                        cur->previous_score = -VALUE_INFINITE;
                        cur->pv = cur->previous_pv;
                        root_move_unset_bound_flags(cur);
                    } else {
                        // Otherwise cap the score to the best possible and mark it
                        // as a bound, which also excuses the incomplete PV.
                        if (prev_is_loss) {
                            cur->score = prev_line->score;
                            cur->uci_score = prev_line->score;
                            cur->previous_score = -VALUE_INFINITE;
                            cur->pv.length = 1;
                            cur->score_upperbound = true;
                        } else {
                            cur->score_upperbound = false;
                        }
                        cur->score_lowerbound = !cur->score_upperbound;
                    }
                }

                // Mark every loss score from a partially searched move as a bound.
                for (size_t li = ctx->pv_idx + 1; li < multi_pv; ++li) {
                    RootMove *const rm = &ctx->root_moves[li];
                    if (root_move_score_is_exact_loss(rm, value_is_loss(rm->score)))
                        rm->score_lowerbound = true;
                }
            }

            stable_sort_root(ctx->root_moves, pv_first, ctx->pv_idx + 1);

            if (main_thread && !id_stopped(ctx)
                && (ctx->pv_idx + 1 == multi_pv || ctx->nodes > ID_NODES_LIMIT_OUTPUT)) {
                search_emit_pv(ctx, ctx->root_depth);
                uci_pv_sent = ctx->pv_idx + 1 == multi_pv;
            }

            if (id_stopped(ctx))
                break;
        }

        // Detect a mate score from an earlier iteration that this one failed to
        // recover. NOT conditioned on `stop`: a COMPLETED iteration that forgets a
        // mate is rolled back too.
        const bool stopped = id_stopped(ctx);
        const int32_t rm0_score = ctx->root_moves[0].score;
        const int32_t abs_rm0 = rm0_score < 0 ? -rm0_score : rm0_score;
        const int32_t abs_last =
          last_best_move_score < 0 ? -last_best_move_score : last_best_move_score;
        const bool forgotten_mate =
          last_best_move_score != -VALUE_INFINITE
          && (value_is_mate(last_best_move_score) || value_is_mated(last_best_move_score))
          && (abs_rm0 < abs_last || root_move_score_is_bound(&ctx->root_moves[0]));

        if (!stopped) {
            if (last_best_move_pv.length == 0
                || ctx->root_moves[0].pv.moves[0] != last_best_move_pv.moves[0])
                last_best_move_depth = ctx->root_depth;

            // Do not replace a (shorter) mate score from a previous iteration.
            if (!forgotten_mate) {
                last_best_move_pv = ctx->root_moves[0].pv;
                last_best_move_score = ctx->root_moves[0].score;
            }
        }

        const bool aborted_loss_search =
          stopped && ctx->pv_idx == 0
          && root_move_score_is_exact_loss(&ctx->root_moves[0],
                                           value_is_loss(ctx->root_moves[0].score));

        // An exact mated-in / TB-loss score from an aborted search cannot be
        // trusted: the loss could be delayed or refuted by the unsearched root
        // moves. Roll back to the previous iteration's score, and do the same when
        // a search has failed to recover a mate found earlier.
        if (aborted_loss_search
            || (ctx->root_moves[0].score != -VALUE_INFINITE && forgotten_mate)) {
            if (last_best_move_pv.length != 0) {
                move_to_front(ctx->root_moves, ctx->root_moves_count, last_best_move_pv.moves[0]);
                ctx->root_moves[0].score = last_best_move_score;
                ctx->root_moves[0].uci_score = last_best_move_score;
                ctx->root_moves[0].pv = last_best_move_pv;
                root_move_unset_bound_flags(&ctx->root_moves[0]);
                if (main_thread)
                    uci_pv_sent = false;
            } else if (aborted_loss_search) {
                // For an aborted depth-1 search label the loss score a lower bound.
                ctx->root_moves[0].score_lowerbound = true;
            }
        }

        // Check whether mate in x is found.
        if (id->limits_mate != 0 && !id_stopped(ctx)
            && ((value_is_mate(ctx->root_moves[0].score)
                 && VALUE_MATE - ctx->root_moves[0].score <= 2 * id->limits_mate)
                || (value_is_mated(ctx->root_moves[0].score)
                    && VALUE_MATE + ctx->root_moves[0].score <= 2 * id->limits_mate)))
            atomic_store_explicit(ctx->stop, true, memory_order_relaxed);

        if (!main_thread)
            continue;

        // Pick a sub-optimal move if the skill level is enabled and time is up.
        if (id->skill_enabled && skill_time_to_pick(id->skill_level, ctx->root_depth))
            skill_best = skill_pick_best(ctx, id, multi_pv);

        tot_best_move_changes += id_collect_bmc(ctx);

        // Manage time: decide whether there is time for the next iteration.
        if (id->use_time_management && !id_stopped(ctx) && id->stop_on_ponderhit
            && !*id->stop_on_ponderhit) {
            const uint64_t nodes_effort =
              ctx->root_moves[0].effort * 100000 / (ctx->nodes > 1 ? ctx->nodes : 1);

            double falling_eval =
              (11.87 + 2.21 * (double) (id->best_previous_average_score - best_value)
               + 1.0 * (double) (id->iter_value[iter_idx] - best_value))
              / 100.0;
            falling_eval = fclamp(falling_eval, 0.572, 1.708);

            const double tr_x = (double) (ctx->root_depth - last_best_move_depth);
            time_reduction = fclamp(0.65 + (1.55 - 0.65) * (tr_x - 5.0) / (18.0 - 5.0), 0.65, 1.55);

            const double reduction =
              (1.48 + id->previous_time_reduction) / (2.157 * time_reduction);
            const double best_move_instability =
              1.096 + 2.29 * tot_best_move_changes / (double) id->threads_size;

            const double hbme_x = (double) (int64_t) nodes_effort;
            const double high_best_move_effort = fclamp(
              0.924 + (0.71 - 0.924) * (hbme_x - 79219.0) / (101822.0 - 79219.0), 0.71, 0.924);

            double total_time = (double) id->tm_optimum * falling_eval * reduction
                              * best_move_instability * high_best_move_effort;
            // Cap used time for a better viewer experience.
            if (ctx->root_moves_count == 1)
                total_time = total_time < 500.0 ? total_time : 500.0;

            // Stop once there is nothing better to find: a mate in <= 3 for us, or
            // a forced mate against us in 2. Without both disjuncts a found mate
            // does not end the iteration, the score stops moving, no time
            // heuristic fires, and the loop grinds to the MAX_PLY ceiling.
            const int32_t mate_in_3 = VALUE_MATE - 3;
            const int32_t mated_in_2 = -VALUE_MATE + 2;
            const bool found_mate = ctx->root_moves[multi_pv - 1].score >= mate_in_3
                                 || ctx->root_moves[0].score == mated_in_2;

            const double elapsed_time = (double) id_elapsed(ctx, id);
            const double cap =
              total_time < (double) id->tm_maximum ? total_time : (double) id->tm_maximum;
            if (elapsed_time > cap || found_mate) {
                if (atomic_load_explicit(id->ponder, memory_order_relaxed))
                    *id->stop_on_ponderhit = true;
                else
                    atomic_store_explicit(ctx->stop, true, memory_order_relaxed);
            } else {
                const bool inc = atomic_load_explicit(id->ponder, memory_order_relaxed)
                              || elapsed_time <= total_time * 0.50;
                atomic_store_explicit(id->increase_depth, inc, memory_order_relaxed);
            }
        }

        id->iter_value[iter_idx] = best_value;
        iter_idx = (iter_idx + 1) & 3;
    }

    if (!main_thread)
        return false;

    id->previous_time_reduction = time_reduction;
    // Swap the best PV line with the sub-optimal one if the skill level is on.
    if (id->skill_enabled) {
        const Move sel = skill_best != MOVE_NONE ? skill_best : skill_pick_best(ctx, id, multi_pv);
        skill_swap_best(ctx, sel);
    }
    return uci_pv_sent;
}
