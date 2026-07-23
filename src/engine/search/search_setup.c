#include "search_setup.h"

#include "option_source.h"
#include "search_common.h"
#include "tt.h"

#include "../board/board_props.h"
#include "../eval/evaluate.h"

#include <string.h>

void search_stack_init(Stack *stack, size_t count, Histories *h, PVMoves *root_pv) {
    memset(stack, 0, count * sizeof *stack);

    SharedStat *const cont_base = cont_hist_page(h, false, false, NO_PIECE, SQ_A1);
    int16_t *const corr_base = cont_corr_page(h, NO_PIECE, SQ_A1);

    // Every frame carries a live continuation page, sentinels included: the
    // six-ply walk and the (ss-4) correction read dereference them unguarded.
    for (size_t i = 0; i < count; ++i) {
        stack[i].continuation_history = cont_base;
        stack[i].continuation_correction_history = corr_base;
    }
    for (size_t k = 0; k < STACK_PAD; ++k)
        stack[k].static_eval = VALUE_NONE;

    for (size_t p = 0; p + STACK_PAD < count && p <= (size_t) MAX_PLY + 2; ++p)
        stack[STACK_PAD + p].ply = (int32_t) p;

    stack[STACK_PAD].pv = root_pv;
    root_pv->length = 0;
}

void search_ctx_init(SearchCtx *ctx,
                     Histories *h,
                     EvalArena *eval_arena,
                     Position *root_pos,
                     const SearchZoneLimits *limits,
                     const RootMoveList *rml,
                     atomic_bool *stop) {
    memset(ctx, 0, sizeof *ctx);

    ctx->hist = h;
    ctx->eval_arena = eval_arena;
    ctx->eval_nnue_ready = eval_arena != nullptr && eval_nnue_available();
    ctx->root_pos = root_pos;
    ctx->stop = stop;
    ctx->limits = *limits;

    ctx->root_moves = rml->moves;
    ctx->root_moves_count = rml->count;
    ctx->tb_config = rml->tb_config;

    ctx->root_delta = 2 * VALUE_INFINITE;  // never zero: reduction_of divides by it
    ctx->reductions = search_reductions_table();
}

void search_tm_init(SearchCtx *ctx, TimeManagement *tm, double *original_time_adjust) {
    SearchZoneLimits *const lim = &ctx->limits;
    const Color us = board_side_to_move(ctx->root_pos);

    const TimemanInput input = {
        .time = lim->time[us],
        .inc = lim->inc[us],
        .start_time = lim->start_time,
        .npmsec = OptionIntByName("nodestime"),
        .move_overhead = OptionIntByName("Move Overhead"),
        .available_nodes = tm->available_nodes,
        .current_optimum_time = tm->optimum_time,
        .current_maximum_time = tm->maximum_time,
        .movestogo = lim->movestogo,
        .ply = board_game_ply(ctx->root_pos),
        .original_time_adjust = *original_time_adjust,
        .ponder = OptionIntByName("Ponder") != 0,
    };

    const TimemanOutput out = timeman_compute(input);

    tm->start_time = out.start_time;
    tm->optimum_time = out.optimum_time;
    tm->maximum_time = out.maximum_time;
    tm->available_nodes = out.available_nodes;
    tm->use_nodes_time = out.use_nodes_time;
    *original_time_adjust = out.original_time_adjust;

    lim->time[us] = out.time;
    lim->inc[us] = out.inc;
    lim->npmsec = out.npmsec;

    tt_new_search();
}

void search_time_state_init(SearchCtx *ctx,
                            const TimeManagement *tm,
                            int *calls_cnt,
                            atomic_bool *ponder,
                            bool *stop_on_ponderhit) {
    ctx->time_state = (SearchTimeState) {
        // Seed from the manager's counter by value; the fast path decrements the
        // ctx field and search_go banks the residue back into the manager.
        .calls_cnt = *calls_cnt,
        .stop_write = ctx->stop,
        .ponder = ponder,
        .stop_on_ponderhit = stop_on_ponderhit,
        .tm_start_time = tm->start_time,
        .tm_maximum_time = tm->maximum_time,
        .lim_nodes = ctx->limits.nodes,
        .lim_movetime = ctx->limits.movetime,
        .tm_use_nodes_time = tm->use_nodes_time,
        .use_time_management = ctx->limits.time[WHITE] != 0 || ctx->limits.time[BLACK] != 0,
    };
    // Leave *calls_cnt alone: upstream's callsCnt persists ACROSS `go` commands
    // (search.cpp:2067 decrements it, ThreadPool::clear resets it to zero on
    // ucinewgame -- thread.cpp:268 -- and nothing touches it per search).
    // search_manager_clear is the ucinewgame reset; a per-go reseed here makes
    // the first time check fire at a different node than upstream's and shifts
    // every `go nodes N` overshoot.
}

double search_skill_level(void) {
    const bool limit_strength = OptionIntByName("UCI_LimitStrength") != 0;
    const int uci_elo = limit_strength ? OptionIntByName("UCI_Elo") : 0;
    if (uci_elo != 0) {
        const double e = (double) (uci_elo - 1320) / (double) (3190 - 1320);
        const double raw = (((37.2473 * e - 40.8525) * e + 22.2943) * e - 0.311438);
        return raw < 0.0 ? 0.0 : (raw > 19.0 ? 19.0 : raw);
    }
    return (double) OptionIntByName("Skill Level");
}

void search_id_state_init(SearchIdState *id,
                          const SearchCtx *ctx,
                          TimeManagement *tm,
                          atomic_bool *ponder,
                          bool *stop_on_ponderhit,
                          atomic_bool *increase_depth) {
    memset(id, 0, sizeof *id);

    id->tm = tm;
    id->ponder = ponder;
    id->stop_on_ponderhit = stop_on_ponderhit;
    id->increase_depth = increase_depth;
    id->previous_time_reduction = 1.0;

    id->thread_idx = 0;
    id->threads_size = 1;
    const int multipv_opt = OptionIntByName("MultiPV");
    id->multipv_option = multipv_opt > 0 ? (size_t) multipv_opt : 0;

    // A sibling has no SearchManager, so TM is null and every field derived from it stays
    // zero. That is safe rather than lucky: the whole time-management block is behind
    // `!main_thread -> continue`, so a worker with no manager never reads one of them.
    // Upstream says the same with a NullSearchManager whose one virtual does nothing.
    if (tm != nullptr) {
        id->tm_optimum = tm->optimum_time;
        id->tm_maximum = tm->maximum_time;
        id->tm_start_time = tm->start_time;
        id->tm_use_nodes_time = tm->use_nodes_time;
    }

    id->limits_depth = ctx->limits.depth;
    id->limits_mate = ctx->limits.mate;

    id->is_main = true;
    id->use_time_management = ctx->limits.time[WHITE] != 0 || ctx->limits.time[BLACK] != 0;

    const double sl = search_skill_level();
    id->skill_level = sl;
    id->skill_enabled = sl < 20.0;
}
