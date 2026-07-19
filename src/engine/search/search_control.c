#include "pool_source.h"
#include "search_control.h"

#include "search_common.h"
#include "time_source.h"

#include <stdatomic.h>

void check_time(SearchCtx *ctx) {
    SearchTimeState *const ts = &ctx->time_state;
    int *const cc = ts->calls_cnt;
    if (!cc)
        return;  // not the main thread => no-op

    *cc -= 1;
    if (*cc > 0)
        return;
    *cc =
      ts->lim_nodes != 0 ? (int) (ts->lim_nodes / 1024 < 512 ? ts->lim_nodes / 1024 : 512) : 512;

    // Read the node count the whole pool has searched. Single-threaded today, so
    // this worker's own count IS the pool's; keep the quantity named for what it
    // is, because gating on a private count is what makes `go nodes N` overshoot
    // once the pool grows (upstream search.cpp:2073, 2088).
    const uint64_t pool_nodes =
      PoolCounters.nodes != nullptr ? PoolCounters.nodes(PoolCounters.ctx) : ctx_nodes(ctx);

    const TimePoint elapsed =
      ts->tm_use_nodes_time ? (TimePoint) pool_nodes : TimeNowMs() - ts->tm_start_time;

    // Load atomically: the UCI thread clears this on ponderhit, and missing that
    // store leaves check_time permanently early-returning, so no time limit is
    // ever enforced.
    if (ts->ponder && atomic_load_explicit(ts->ponder, memory_order_relaxed))
        return;

    const bool over_management =
      ts->use_time_management
      && (elapsed > ts->tm_maximum_time || (ts->stop_on_ponderhit && *ts->stop_on_ponderhit));
    const bool over_movetime = ts->lim_movetime != 0 && elapsed >= ts->lim_movetime;
    const bool over_nodes = ts->lim_nodes != 0 && pool_nodes >= ts->lim_nodes;

    if (over_management || over_movetime || over_nodes)
        atomic_store_explicit(ts->stop_write, true, memory_order_relaxed);
}

// Sentinel the mean-squared score starts at (-VALUE_INFINITE^2). Held as an int
// because that is the width upstream keeps it in.
static const int32_t RootMeanSqSentinel = -(VALUE_INFINITE * VALUE_INFINITE);

void root_update(SearchCtx *ctx,
                 Move move,
                 Value value,
                 uint64_t nodes_delta,
                 int move_count,
                 Value alpha,
                 Value beta,
                 const PVMoves *child_pv) {
    size_t idx = ctx->pv_idx;
    const size_t last = ctx->pv_last;
    while (idx < last && ctx->root_moves[idx].pv.moves[0] != move)
        ++idx;
    RootMove *const rm = &ctx->root_moves[idx];

    rm->effort += nodes_delta;

    // Dynamic EMA (upstream 93ed4b53c): weight this move's node share (N) against
    // its prior effort (E_prev). The averageScore / meanSquaredScore updates run
    // in u64 as upstream does — `value * w` promotes the signed value to u64, so
    // this is UNSIGNED wrapping arithmetic truncated back to i32, and it is
    // bit-exact only when replicated exactly.
    const uint64_t scale = 32;
    const uint64_t n = nodes_delta;
    const uint64_t effort_prev = rm->effort - n > 1 ? rm->effort - n : 1;
    uint64_t w = scale * n * 2 / (n * 2 + 3 * effort_prev);
    w = w < 12 ? 12 : (w > 24 ? 24 : w);
    const uint64_t w_mss = w < 16 ? w : 16;

    const int32_t av = value < 0 ? -value : value;
    const int64_t v2 = (int64_t) value * (int64_t) av;

    if (rm->average_score == -VALUE_INFINITE) {
        rm->average_score = value;
    } else {
        const uint64_t value_u = (uint64_t) (int64_t) value;
        const uint64_t avg_u = (uint64_t) (int64_t) rm->average_score;
        rm->average_score = (int32_t) (uint32_t) ((value_u * w + avg_u * (scale - w)) / scale);
    }

    if (rm->mean_squared_score == RootMeanSqSentinel) {
        rm->mean_squared_score = value * av;
    } else {
        const uint64_t v2_u = (uint64_t) v2;
        const uint64_t mss_u = (uint64_t) (int64_t) rm->mean_squared_score;
        rm->mean_squared_score =
          (int32_t) (uint32_t) ((v2_u * w_mss + mss_u * (scale - w_mss)) / scale);
    }

    if (move_count == 1 || value > alpha) {
        rm->score = value;
        rm->uci_score = value;
        rm->sel_depth = ctx->sel_depth;
        rm->score_lowerbound = false;
        rm->score_upperbound = false;
        if (value >= beta) {
            rm->score_lowerbound = true;
            rm->uci_score = beta;
        } else if (value <= alpha) {
            rm->score_upperbound = true;
            rm->uci_score = alpha;
        }
        // Keep pv[0] (== move) with pv.resize(1), then append the child PV.
        rm->pv.length = 1;
        if (child_pv) {
            for (size_t j = 0; j < child_pv->length; ++j)
                rm->pv.moves[1 + j] = child_pv->moves[j];
            rm->pv.length = 1 + child_pv->length;
        }
        if (move_count > 1 && ctx->pv_idx == 0)
            ctx_set_best_move_changes(ctx, ctx_best_move_changes(ctx) + 1);
    } else {
        rm->score = -VALUE_INFINITE;
    }
}

bool root_in_list(const SearchCtx *ctx, Move move) {
    for (size_t i = ctx->pv_idx; i < ctx->pv_last; ++i)
        if (ctx->root_moves[i].pv.moves[0] == move)
            return true;
    return false;
}

bool search_stopped(const SearchCtx *ctx) {
    return atomic_load_explicit(ctx->stop, memory_order_relaxed);
}
