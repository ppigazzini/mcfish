#include "pool_source.h"
#include "search_emit.h"

#include "option_source.h"
#include "output_sink.h"
#include "syzygy_pv.h"
#include "time_source.h"
#include "tt.h"
#include "uci_wdl.h"

#include "../board/board_props.h"
#include "../board/score.h"
#include "../board/uci_move.h"

#include <string.h>

// Report the POOL's totals, as upstream's output_pv does (search.cpp:2235, :2239). A
// null hook is a caller with no pool, which reads as this worker's own -- and at one
// thread the two are the same number.
static uint64_t pool_nodes(const SearchCtx *ctx) {
    return PoolCounters.nodes != nullptr ? PoolCounters.nodes(PoolCounters.ctx) : ctx_nodes(ctx);
}

static uint64_t pool_tb_hits(const SearchCtx *ctx) {
    return PoolCounters.tb_hits != nullptr ? PoolCounters.tb_hits(PoolCounters.ctx)
                                           : ctx_tb_hits(ctx);
}


enum { PV_TEXT_MAX = 4096, LINE_MAX = 5120 };

static void emit(const char *line) { OutputPrintLine(line, strlen(line)); }

// Format the score text through the classifier: a real mate prints as plies to
// mate, a tablebase result as the fixed TB centipawn band, anything else as the
// win-rate-normalised centipawn value.
static void score_text(int v, int material, char *buf, size_t n) {
    const ScoreClass sc = score_classify(v, VALUE_TB_WIN_IN_MAX_PLY, VALUE_TB, VALUE_MATE);
    switch (sc.kind) {
    case SCORE_MATE :
        uci_format_score(UCI_SCORE_MATE, sc.plies, 0, buf, n);
        break;
    case SCORE_TABLEBASE :
        uci_format_score(UCI_SCORE_TABLEBASE, sc.plies, sc.win ? 1 : 0, buf, n);
        break;
    default :
        uci_format_score(UCI_SCORE_CP, uci_wdl_to_cp(v, material), 0, buf, n);
        break;
    }
}

// Render a PV into BUF as space-separated UCI moves.
static void render_pv(const Position *pos, const PVMoves *pv, char *buf, size_t n) {
    size_t used = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < pv->length; ++i) {
        char mbuf[8];
        move_to_uci(pos, pv->moves[i], mbuf);
        const size_t len = strlen(mbuf);
        if (used + len + 2 >= n)
            break;
        if (i != 0)
            buf[used++] = ' ';
        memcpy(buf + used, mbuf, len);
        used += len;
        buf[used] = '\0';
    }
}

// Mirror upstream is_mate_or_mated: |v| >= VALUE_MATE_IN_MAX_PLY, i.e. a real
// mate and not a TB win. A genuine mate score is never overridden by tbScore.
static bool is_mate_or_mated(int v) {
    return v >= VALUE_MATE_IN_MAX_PLY || v <= VALUE_MATED_IN_MAX_PLY;
}

void search_emit_pv(SearchCtx *ctx, int depth) {
    OutputSetLastNodesSearched(pool_nodes(ctx));
    if (OutputIsQuiet())
        return;

    const Position *const pos = ctx->root_pos;
    const int material = board_wdl_material(pos);
    const bool show_wdl = OptionIntByName("UCI_ShowWDL") != 0;

    const int multipv_opt = OptionIntByName("MultiPV");
    size_t multipv = multipv_opt > 0 ? (size_t) multipv_opt : 0;
    if (multipv > ctx->root_moves_count)
        multipv = ctx->root_moves_count;

    // Report the root-ranking probes as one hit per root move, as upstream does.
    const uint64_t tb_hits =
      pool_tb_hits(ctx) + (ctx->tb_config.root_in_tb ? ctx->root_moves_count : 0);
    const TimePoint raw_elapsed = TimeNowMs() - ctx->time_state.tm_start_time;
    const uint64_t elapsed_ms = (uint64_t) (raw_elapsed > 1 ? raw_elapsed : 1);
    const uint64_t nps = pool_nodes(ctx) * 1000 / elapsed_ms;
    const int hashfull = tt_hashfull(0);

    for (size_t i = 0; i < multipv; ++i) {
        RootMove *const rm = &ctx->root_moves[i];
        const bool use_prev = rm->score == -VALUE_INFINITE;
        if (depth == 1 && use_prev && i > 0)
            continue;

        const int d = use_prev ? (depth - 1 > 1 ? depth - 1 : 1) : depth;
        int v = use_prev ? rm->previous_score : rm->uci_score;
        if (v == -VALUE_INFINITE)
            v = 0;

        // With the root in a tablebase and no real mate on the board, show the
        // exact tbScore rather than the search score.
        const bool is_tb_score = ctx->tb_config.root_in_tb && !is_mate_or_mated(v);
        if (is_tb_score)
            v = rm->tb_score;

        // Correct and extend the PV, and in exceptional cases V. A PV carried over
        // from the previous iteration was already extended when it was emitted, and
        // a bound flag means the search never proved this line — so neither is
        // touched, unless the score came from the tablebase and is exact anyway.
        if (value_is_decisive(v) && !is_mate_or_mated(v) && !use_prev
            && (!root_move_score_is_bound(rm) || is_tb_score))
            syzygy_extend_pv(ctx->root_pos, ctx->time_state.use_time_management, rm, &v);

        const char *bound_text = "";
        if (!use_prev && !is_tb_score) {
            if (rm->score_lowerbound)
                bound_text = "lowerbound";
            else if (rm->score_upperbound)
                bound_text = "upperbound";
        }

        char sbuf[48];
        score_text(v, material, sbuf, sizeof sbuf);

        char wbuf[32] = "";
        if (show_wdl)
            uci_wdl_text(v, material, wbuf, sizeof wbuf);

        // Report the PV that belongs to the score being reported. A root move still
        // carrying the previous iteration's score has a `pv` the current iteration
        // never verified, so upstream selects previousPV in exactly that case
        // (Stockfish/src/search.cpp:2262). mcfish already maintained previous_pv and
        // read it in two other places; only the emitter was reading past it.
        char pv_text[PV_TEXT_MAX];
        render_pv(pos, use_prev ? &rm->previous_pv : &rm->pv, pv_text, sizeof pv_text);

        char line[LINE_MAX];
        uci_format_info_full(d, rm->sel_depth, i + 1, sbuf, bound_text, wbuf, show_wdl,
                             pool_nodes(ctx), nps, hashfull, tb_hits, elapsed_ms, pv_text, line,
                             sizeof line);
        emit(line);
    }
}

void search_emit_no_moves(const Position *pos) {
    if (OutputIsQuiet())
        return;

    const int v = board_has_checkers(pos) ? -VALUE_MATE : 0;
    char sbuf[48];
    score_text(v, board_wdl_material(pos), sbuf, sizeof sbuf);

    char line[128];
    uci_format_info_no_moves(0, sbuf, line, sizeof line);
    emit(line);
    emit("bestmove (none)");
}

void search_emit_bestmove(const Position *pos, const RootMove *best) {
    if (OutputIsQuiet())
        return;

    char bm[8];
    move_to_uci(pos, best->pv.moves[0], bm);

    char ponder[8] = "";
    if (best->pv.length > 1)
        move_to_uci(pos, best->pv.moves[1], ponder);

    char line[48];
    uci_format_bestmove(bm, ponder, line, sizeof line);
    emit(line);
}

void search_emit_root_on_iter(const SearchCtx *ctx, int depth, Move move, int move_count) {
    if (OutputIsQuiet())
        return;

    char mbuf[8];
    move_to_uci(ctx->root_pos, move, mbuf);

    char line[96];
    uci_format_info_iter(depth, mbuf, move_count + (int) ctx->pv_idx, line, sizeof line);
    emit(line);
}
