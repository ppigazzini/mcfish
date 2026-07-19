#include "search.h"

#include "../../platform/clock.h"
#include "../board/board_props.h"
#include "../board/movegen.h"
#include "../board/uci_move.h"
#include "../eval/evaluate.h"
#include "history.h"
#include "option_source.h"
#include "output_sink.h"
#include "root_move_build.h"
#include "search_emit.h"
#include "search_id.h"
#include "search_setup.h"
#include "search_types.h"
#include "time_source.h"
#include "timeman.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

// Drive one search over the decomposed search zone.
//
// This file is the facade `search.h` promises and nothing else: it maps the
// shell-facing SearchLimits onto the zone's SearchZoneLimits, registers the
// injection seams, builds the root move list, and hands the whole thing to
// `iterative_deepening`. Every node body, every pruning margin and every info
// line lives in the zone modules — nothing here may re-derive any of them, or the
// facade becomes a second search that drifts from the first.
//
// Golden: `Stockfish/src/search.cpp: Search::Worker::start_searching`.
// Port source: zfish `engine/search/search_driver.zig: workerStartSearching` and
// `engine/search/headless_search.zig` (the single-worker driver shape).

// ---- the shell-facing sink ---------------------------------------------

static void (*Emit)(const char *line) = nullptr;

void search_set_output(void (*emit)(const char *line)) { Emit = emit; }

// Bound the copy at the widest line search_emit builds, so a line that outgrows
// this buffer is truncated rather than read past its end.
enum { EMIT_LINE_MAX = 5120 };

static void facade_print_line(const char *str, size_t len) {
    if (!Emit)
        return;
    char line[EMIT_LINE_MAX];
    const size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, str, n);
    line[n] = '\0';
    Emit(line);
}

// Report quiet exactly when no sink is installed. A headless caller (the test
// binary) then runs the whole formatting path without writing anywhere.
static bool facade_is_quiet(void) { return Emit == nullptr; }

static uint64_t LastNodesSearched = 0;
static void facade_set_last_nodes(uint64_t nodes) { LastNodesSearched = nodes; }

// Carry the per-game manager scalars ACROSS `go` commands. Upstream resets these
// in ThreadPool::clear, which runs on `ucinewgame` and nowhere else, so within a
// game each search starts from the previous one's result. Resetting them per `go`
// searches a different tree -- and because bench drives 54 positions behind a
// single ucinewgame, it changes the anchor by percent, not by nodes.
static Value BestPreviousScore = VALUE_INFINITE;
static Value BestPreviousAverageScore = VALUE_INFINITE;
static double PreviousTimeReduction = 0.85;

void search_clear(void) {
    history_clear(histories());
    eval_nnue_clear_refresh_cache();
    BestPreviousScore = VALUE_INFINITE;
    BestPreviousAverageScore = VALUE_INFINITE;
    PreviousTimeReduction = 0.85;
}

uint64_t search_last_nodes_searched(void) { return LastNodesSearched; }
void search_reset_last_nodes_searched(void) { LastNodesSearched = 0; }

// ---- the option seam ----------------------------------------------------
//
// Answer with upstream's defaults for the options that steer the search. The
// live shell carries no option model yet, and the zone's own fallback answers 0
// to everything — which would read as MultiPV 0 (no PV line searched at all) and
// Skill Level 0 (maximum handicap). Both are wrong searches rather than absent
// ones, so the facade owns the defaults until the decomposed shell registers the
// real model. Golden: `Stockfish/src/engine.cpp` (the option table's defaults).

static int facade_option_int(const char *name) {
    if (strcmp(name, "MultiPV") == 0)
        return 1;
    if (strcmp(name, "Skill Level") == 0)
        return 20;
    if (strcmp(name, "Move Overhead") == 0)
        return 10;
    return 0;
}

static int64_t facade_now_ms(void) { return (int64_t) now_ms(); }

// Register the seams the zone reads. Idempotent, and run before every search so a
// caller that never touches the shell still gets a correct clock and option set.
static void install_seams(void) {
    OutputPrintLine = facade_print_line;
    OutputIsQuiet = facade_is_quiet;
    OutputSetLastNodesSearched = facade_set_last_nodes;
    OptionIntByName = facade_option_int;
    TimeNowMs = facade_now_ms;
}

// ---- per-search state ---------------------------------------------------
//
// Hold what upstream keeps on the SearchManager. Upstream resets all of it in
// `ThreadPool::clear` (thread.cpp), on `ucinewgame`; ccfish's shell has no such
// hook, so the facade resets it per `go` alongside the history block, which is
// also what keeps two searches of the same position node-for-node identical.

static atomic_bool Stop = false;
static atomic_bool Ponder = false;
static atomic_bool IncreaseDepth = true;
static bool StopOnPonderhit = false;
static int CallsCnt = 0;
static TimeManagement Tm = { .available_nodes = -1 };

static double OriginalTimeAdjust = -1.0;

void search_stop(void) { atomic_store(&Stop, true); }

// Translate the shell's limit set into the field set the zone reads. START is the
// clock reading that anchors the whole search: it must be taken once, here.
static SearchZoneLimits to_zone_limits(const SearchLimits *limits, TimePoint start) {
    return (SearchZoneLimits) {
        .time = { (TimePoint) limits->time_ms[WHITE], (TimePoint) limits->time_ms[BLACK] },
        .inc = { (TimePoint) limits->inc_ms[WHITE], (TimePoint) limits->inc_ms[BLACK] },
        .npmsec = 0,
        .movetime = (TimePoint) limits->movetime_ms,
        .start_time = start,
        .movestogo = limits->moves_to_go,
        .depth = limits->depth,
        .mate = 0,
        .perft = 0,
        .infinite = limits->infinite,
        .nodes = limits->nodes,
        .ponder = limits->ponder,
    };
}

// Fill the caller's result from the searched root move list.
static SearchResult result_of(const SearchCtx *ctx, TimePoint elapsed) {
    const RootMove *const best = &ctx->root_moves[0];
    return (SearchResult) {
        .nodes = ctx->nodes,
        .depth_reached = ctx->root_depth,
        .score = (Value) best->score,
        .best_move = best->pv.moves[0],
        .ponder_move = best->pv.length > 1 ? best->pv.moves[1] : MOVE_NONE,
        .elapsed_ms = (int) elapsed,
    };
}

// Keep the context off the C stack: SearchCtx carries the reductions table and a
// full PV buffer, and `iterative_deepening` puts a whole Stack array beside it.
static SearchCtx Ctx;

SearchResult search_go(Position *pos, const SearchLimits *limits) {
    install_seams();

    const TimePoint start = (TimePoint) now_ms();

    atomic_store(&Stop, false);
    atomic_store(&Ponder, limits->ponder);
    atomic_store(&IncreaseDepth, true);
    StopOnPonderhit = false;
    CallsCnt = 0;
    Tm = (TimeManagement) { .available_nodes = -1 };
    OriginalTimeAdjust = -1.0;

    // Drop the accumulator to one uncomputed root slot, so the first evaluation
    // refreshes from this board rather than from the previous search's diffs.
    // Once per `go`, not once per iteration (zfish `search_driver.zig: ssPrologue`).
    eval_acc_reset();


    Histories *const h = histories();

    SearchResult result = { .score = VALUE_ZERO, .best_move = MOVE_NONE };

    ExtMove legal[MAX_MOVES];
    const size_t count = (size_t) (generate_legal(pos, legal) - legal);
    if (count == 0) {
        result.score = board_has_checkers(pos) ? mated_in(0) : VALUE_DRAW;
        search_emit_no_moves(pos);
        return result;
    }

    Move moves[MAX_MOVES];
    for (size_t i = 0; i < count; ++i)
        moves[i] = legal[i].move;

    // Hand the root FEN to the ranking: it replays each root move from that string
    // on a scratch board. Only the tablebase path reads it, which is inert here.
    char root_fen[128];
    pos_fen(pos, root_fen);

    RootMoveList rml;
    if (!root_moves_build(pos, root_fen, board_is_chess960(pos), moves, count, &rml)) {
        // An allocation failure leaves nothing to search. Return a legal move
        // rather than MOVE_NONE, so the caller still has something playable.
        result.best_move = legal[0].move;
        return result;
    }

    const SearchZoneLimits zone_limits = to_zone_limits(limits, start);

    search_ctx_init(&Ctx, h, pos, &zone_limits, &rml, &Stop);
    search_tm_init(&Ctx, &Tm, &OriginalTimeAdjust);
    search_time_state_init(&Ctx, &Tm, &CallsCnt, &Ponder, &StopOnPonderhit);

    SearchIdState id;
    search_id_state_init(&id, &Ctx, &Tm, &Ponder, &StopOnPonderhit, &IncreaseDepth);

    // Seed the per-game manager scalars search_id_state_init leaves at zero.
    // VALUE_INFINITE is upstream's "no previous score", and the time-reduction
    // seed is upstream's own (Stockfish/src/thread.cpp: ThreadPool::clear).
    id.best_previous_score = BestPreviousScore;
    id.best_previous_average_score = BestPreviousAverageScore;
    id.previous_time_reduction = PreviousTimeReduction;

    const bool uci_pv_sent = iterative_deepening(&Ctx, &id);

    // Record what this search concluded, so the next `go` in this game seeds its
    // aspiration window and its falling-eval term from it. Upstream assigns these
    // in the driver, after the depth loop returns (search.cpp:245-246) -- which is
    // why iterative_deepening only ever reads them. Without this they stay at the
    // VALUE_INFINITE seed forever: inert under `go depth N`, but at any real time
    // control it costs the window seed and skews falling_eval every move.
    id.best_previous_score = Ctx.root_moves[0].score;
    id.best_previous_average_score = Ctx.root_moves[0].average_score;

    // Publish the scalars for the next `go` in this game.
    BestPreviousScore = id.best_previous_score;
    BestPreviousAverageScore = id.best_previous_average_score;
    PreviousTimeReduction = id.previous_time_reduction;

    // Report the finished line once, and only once: the depth loop already emits
    // it when the last MultiPV line of the final iteration completed.
    if (!uci_pv_sent)
        search_emit_pv(&Ctx, Ctx.root_depth);
    search_emit_bestmove(pos, &Ctx.root_moves[0]);

    result = result_of(&Ctx, (TimePoint) now_ms() - start);
    root_moves_free(&rml);
    Ctx.root_moves = nullptr;
    Ctx.root_moves_count = 0;
    return result;
}

uint64_t perft(Position *pos, int depth, bool root) {
    ExtMove list[MAX_MOVES];
    const int count = (int) (generate_legal(pos, list) - list);

    // Bulk-count at depth 1 only off the root: the root still walks each move so
    // the per-move split it prints stays comparable with upstream's `go perft`.
    if (depth <= 1 && !root)
        return (uint64_t) count;

    uint64_t total = 0;
    for (int i = 0; i < count; ++i) {
        uint64_t n = 1;

        if (depth > 1) {
            StateInfo st;
            pos_do_move(pos, list[i].move, &st, false, &pos->scratch_dp, &pos->scratch_dts);
            n = perft(pos, depth - 1, false);
            pos_undo_move(pos, list[i].move);
        }
        total += n;

        if (root && Emit) {
            char buf[8], line[64];
            move_to_uci(pos, list[i].move, buf);
            snprintf(line, sizeof line, "%s: %llu", buf, (unsigned long long) n);
            Emit(line);
        }
    }
    return total;
}
