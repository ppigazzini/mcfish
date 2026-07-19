#include "search.h"

#include "../../platform/clock.h"
#include "../board/board_props.h"
#include "../board/movegen.h"
#include "../board/uci_move.h"
#include "../eval/evaluate.h"
#include "../state/worker_construct.h"
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

// ---- the worker ---------------------------------------------------------
//
// Hold the one worker this facade drives. Every per-worker field the search used to
// keep as a file-scope static -- the history tables, the NNUE arena, the SearchCtx --
// lives in this block, and every per-game manager scalar lives in its SearchManager.
//
// The manager scalars carry ACROSS `go` commands. Upstream resets them in
// ThreadPool::clear, which runs on `ucinewgame` and nowhere else, so within a game each
// search starts from the previous one's result. Resetting them per `go` searches a
// different tree -- and because bench drives its position list behind a single
// ucinewgame, that changes the anchor by percent, not by nodes.
//
// The time manager carries across for the same reason and upstream clears it in the
// same place (thread.cpp:266-271). It only steers the clock, so it does not move the
// depth-limited anchor -- but resetting `available_nodes` per `go` hands `nodestime` a
// fresh budget every move instead of one per game, and resetting the adjust makes every
// move take the first-move-of-a-game path.
static SearchWorker *MainWorker = nullptr;

// Build the worker on first use. Bound to the process-wide single-thread history bank,
// so its table sizes -- and every index mask the search takes -- are the one-thread ones.
static SearchWorker *main_worker(void) {
    if (MainWorker != nullptr)
        return MainWorker;

    Histories *const h = histories();
    if (h == nullptr)
        return nullptr;

    const WorkerCtorInputs in = {
        .shared_history = h->shared,
        .threads = nullptr,
        .thread_idx = 0,
        .numa_thread_idx = 0,
        .numa_total = 1,
        .numa_access_token = 0,
    };
    MainWorker = worker_create(&in);
    return MainWorker;
}

void search_clear(void) {
    SearchWorker *const w = main_worker();
    if (w != nullptr)
        worker_clear(w);
}

void search_shutdown(void) {
    worker_destroy(MainWorker);
    MainWorker = nullptr;
    histories_shutdown();
}

uint64_t search_last_nodes_searched(void) { return LastNodesSearched; }
void search_reset_last_nodes_searched(void) { LastNodesSearched = 0; }

// ---- the option seam ----------------------------------------------------
//
// Answer with upstream's defaults for the options that steer the search. The
// zone's own fallback answers 0 to everything — which would read as MultiPV 0
// (no PV line searched at all) and Skill Level 0 (maximum handicap). Both are
// wrong searches rather than absent ones, so the facade owns the defaults for
// any caller that drives search_go without an option table: the bench harness
// and the unit tests both do. Golden: `Stockfish/src/engine.cpp` (the option
// table's defaults).

static int facade_option_int(const char *name) {
    if (strcmp(name, "MultiPV") == 0)
        return 1;
    if (strcmp(name, "Skill Level") == 0)
        return 20;
    if (strcmp(name, "Move Overhead") == 0)
        return 10;
    return 0;
}

// Hold the shell's option table when one is installed. install_seams runs before
// every search, so without this indirection it would overwrite the shell's
// registration on the first `go` and every UCI option would silently revert to
// the facade defaults above.
static int (*ShellOptionInt)(const char *name) = nullptr;

void search_set_option_source(int (*option_int_by_name)(const char *name)) {
    ShellOptionInt = option_int_by_name;
}

static int64_t facade_now_ms(void) { return (int64_t) now_ms(); }

// Register the seams the zone reads. Idempotent, and run before every search so a
// caller that never touches the shell still gets a correct clock and option set.
static void install_seams(void) {
    OutputPrintLine = facade_print_line;
    OutputIsQuiet = facade_is_quiet;
    OutputSetLastNodesSearched = facade_set_last_nodes;
    OptionIntByName = ShellOptionInt ? ShellOptionInt : facade_option_int;
    TimeNowMs = facade_now_ms;
}

// ---- per-search state ---------------------------------------------------
//
// Hold the flags one `go` owns outright. Everything upstream resets only in
// `ThreadPool::clear` lives in the per-game block above instead; these are the
// fields a fresh search genuinely re-arms, so resetting them per `go` is what
// keeps two searches of the same position node-for-node identical.

static atomic_bool Stop = false;
static atomic_bool IncreaseDepth = true;

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

SearchResult search_go(Position *pos, const SearchLimits *limits) {
    install_seams();

    const TimePoint start = (TimePoint) now_ms();

    atomic_store(&Stop, false);
    atomic_store(&IncreaseDepth, true);

    SearchResult result = { .score = VALUE_ZERO, .best_move = MOVE_NONE };

    ExtMove legal[MAX_MOVES];
    const size_t count = (size_t) (generate_legal(pos, legal) - legal);

    SearchWorker *const w = main_worker();
    if (w == nullptr) {
        // Neither the history bank nor the evaluation arena could be allocated, so there
        // is nothing to search against. Return a legal move rather than MOVE_NONE, as
        // the root-move allocation failure below does.
        result.best_move = count != 0 ? legal[0].move : MOVE_NONE;
        return result;
    }

    // Re-seed the refresh cache when the net has changed since this worker last did.
    // The worker is built before the shell loads the net, so the first `go` is what
    // seeds it; an EvalFile change reloads under a worker that already exists.
    worker_ensure_network(w);

    SearchManager *const sm = w->manager;
    SearchCtx *const ctx = &w->ctx;

    atomic_bool_store(&sm->ponder, limits->ponder);
    sm->stop_on_ponderhit = false;
    sm->calls_cnt = 0;

    // Drop the accumulator to one uncomputed root slot, so the first evaluation
    // refreshes from this board rather than from the previous search's diffs.
    // Once per `go`, not once per iteration.
    eval_acc_reset(w->eval_arena);

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

    if (!root_moves_build(pos, root_fen, board_is_chess960(pos), moves, count, &w->rml)) {
        // An allocation failure leaves nothing to search. Return a legal move
        // rather than MOVE_NONE, so the caller still has something playable.
        result.best_move = legal[0].move;
        return result;
    }

    const SearchZoneLimits zone_limits = to_zone_limits(limits, start);

    search_ctx_init(ctx, &w->hist, w->eval_arena, pos, &zone_limits, &w->rml, &Stop);
    search_tm_init(ctx, &sm->tm, &sm->original_time_adjust);
    search_time_state_init(ctx, &sm->tm, &sm->calls_cnt, &sm->ponder.value, &sm->stop_on_ponderhit);

    SearchIdState id;
    search_id_state_init(&id, ctx, &sm->tm, &sm->ponder.value, &sm->stop_on_ponderhit,
                         &IncreaseDepth);

    // Seed the per-game manager scalars search_id_state_init leaves at zero.
    id.best_previous_score = sm->best_previous_score;
    id.best_previous_average_score = sm->best_previous_average_score;
    id.previous_time_reduction = sm->previous_time_reduction;

    const bool uci_pv_sent = iterative_deepening(ctx, &id);

    // In `nodes as time` mode, subtract what this search spent from the budget
    // before returning (Stockfish/src/search.cpp:235-237). Read the limit here,
    // not from the snapshot taken before `search_tm_init`: that call is what
    // writes `npmsec` from the `nodestime` option, and it also scales `inc` by
    // it, so both fields only hold the values upstream subtracts once it has run.
    if (ctx->limits.npmsec != 0)
        timeman_advance_nodes_time(&sm->tm,
                                   (int64_t) ctx->nodes - ctx->limits.inc[board_side_to_move(pos)]);

    // Record what this search concluded, so the next `go` in this game seeds its
    // aspiration window and its falling-eval term from it. Upstream assigns these
    // in the driver, after the depth loop returns (search.cpp:245-246) -- which is
    // why iterative_deepening only ever reads them. Without this they stay at the
    // VALUE_INFINITE seed forever: inert under `go depth N`, but at any real time
    // control it costs the window seed and skews falling_eval every move.
    sm->best_previous_score = ctx->root_moves[0].score;
    sm->best_previous_average_score = ctx->root_moves[0].average_score;
    sm->previous_time_reduction = id.previous_time_reduction;

    // Report the finished line once, and only once: the depth loop already emits
    // it when the last MultiPV line of the final iteration completed.
    if (!uci_pv_sent)
        search_emit_pv(ctx, ctx->root_depth);
    search_emit_bestmove(pos, &ctx->root_moves[0]);

    result = result_of(ctx, (TimePoint) now_ms() - start);
    root_moves_free(&w->rml);
    ctx->root_moves = nullptr;
    ctx->root_moves_count = 0;
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
