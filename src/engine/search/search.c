#include "search.h"

#include "../../platform/clock.h"
#include "../board/board_props.h"
#include "../board/legality.h"
#include "../board/movegen.h"
#include "../board/uci_move.h"
#include "../eval/evaluate.h"
#include "../state/worker_construct.h"
#include "search_threads.h"
#include "history.h"
#include "option_source.h"
#include "pool_source.h"
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
#include <stdlib.h>
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

// Reach the pool and the worker set through the Lazy-SMP driver. The pool holds the ONLY
// copy of `stop` and `increase_depth`: one flag with one writer and many relaxed readers
// is the whole cross-thread protocol, and a second copy is how the siblings come to
// disagree about whether a search is still running.
//
// Expose the two as plain `atomic_bool *`, which is what the zone's SearchCtx and
// SearchIdState hold. The address is the pool's own storage, so a write through either is
// the write every worker polls.
static atomic_bool *pool_stop(void) { return &search_threads_pool()->stop.value; }
static atomic_bool *pool_increase_depth(void) {
    return &search_threads_pool()->increase_depth.value;
}

void search_stop(void) { thread_pool_set_stop(search_threads_pool(), true); }

bool search_set_threads(size_t count) { return search_threads_set(count); }
bool search_set_numa_policy(const char *policy) { return search_threads_set_numa_policy(policy); }

// Clear every worker's tables, as upstream's ThreadPool::clear does. The per-game
// manager scalars are the SearchManager's, so `worker_clear` resets them with the rest --
// they carry ACROSS `go` commands within a game, which is why this is reached from
// `ucinewgame` and nowhere else. Resetting them per `go` searches a different tree, and
// because bench drives its position list behind a single ucinewgame that would change the
// anchor by percent, not by nodes.
void search_clear(void) {
    if (search_threads_main() != nullptr)
        search_threads_clear();
}

void search_shutdown(void) {
    search_threads_shutdown();
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
        .nodes = ctx_nodes(ctx),
        .depth_reached = ctx->root_depth,
        .score = (Value) best->score,
        .best_move = best->pv.moves[0],
        .ponder_move = best->pv.length > 1 ? best->pv.moves[1] : MOVE_NONE,
        .elapsed_ms = (int) elapsed,
    };
}

// Set one worker up on the root: its own board, its own copy of the ranked root move
// list, its own zeroed counters, and its SearchCtx bound to its own tables and arena.
//
// The board is rebuilt from the caller's FEN and then has the caller's root state copied
// over it, which is upstream's own two-step (thread.cpp: rootPos.set(...) followed by
// rootState = setupStates->back()). The copy is what carries the game's repetition chain:
// `previous` still points into the shell's state list, which no worker writes during a
// search.
static bool worker_root_setup(SearchWorker *w,
                              const Position *pos,
                              const char *root_fen,
                              const RootMoveList *src,
                              const SearchZoneLimits *zone_limits) {
    if (w->rml.moves == nullptr || w->rml.count != src->count) {
        root_moves_free(&w->rml);
        w->rml.moves = calloc(src->count, sizeof *w->rml.moves);
        if (w->rml.moves == nullptr)
            return false;
    }
    memcpy(w->rml.moves, src->moves, src->count * sizeof *src->moves);
    w->rml.count = src->count;
    w->rml.tb_config = src->tb_config;

    if (!pos_set(&w->root_pos, root_fen, board_is_chess960(pos), &w->root_state))
        return false;
    w->root_state = *pos->st;
    w->root_pos.st = &w->root_state;

    worker_ensure_network(w);
    eval_acc_reset(w->eval_arena);

    search_ctx_init(&w->ctx, &w->hist, w->eval_arena, &w->root_pos, zone_limits, &w->rml,
                    pool_stop());
    return true;
}

// Run one sibling's whole search. Every field it touches is its own -- upstream's Lazy-SMP
// shares the transposition table and the node's history bank and nothing else.
static void sibling_search(void *ctx) {
    SearchWorker *const w = (SearchWorker *) ctx;
    if (w == nullptr || w->ctx.root_moves == nullptr)
        return;

    SearchIdState id;
    // Pass no manager: a sibling has none, so check_time returns before touching any of
    // its fields and the emit path stays silent. That is upstream's NullSearchManager.
    search_id_state_init(&id, &w->ctx, nullptr, nullptr, nullptr, pool_increase_depth());
    id.thread_idx = w->thread_idx;
    id.is_main = false;
    (void) iterative_deepening(&w->ctx, &id);
}

// ---- the async search session -------------------------------------------
//
// One search runs at a time -- a single UCI session drives one board -- so the
// state a dispatched worker-0 job needs once search_go_start has returned lives
// on one static block, not a heap handoff. The dispatch/join handshake in
// thread.c orders every write here (done in _start, before the dispatch) ahead
// of every read in the job, and the job's writes ahead of search_wait's read, so
// no field but `running` is ever touched by two threads at once. `running` IS,
// so it is the one atomic: the guard a second `go` reads to refuse re-entry while
// a search is still in flight.
typedef struct {
    SearchWorker *w;    // worker 0
    SearchManager *sm;  // worker 0's manager
    SearchIdState id;   // thread 0's driver state, outliving _start's frame
    TimePoint start;    // clock stamped when `go` was parsed
    size_t threads;
    int limit_depth;      // limits->depth, read by the vote gate in the job
    bool unbounded;       // infinite/ponder: has no self-termination, so `quit` must stop it
    SearchResult result;  // published by the job, read after search_wait
    AtomicBool running;
} SearchSession;

static SearchSession Session;
static bool SessionReady = false;

static void session_ensure(void) {
    if (!SessionReady) {
        atomic_bool_init(&Session.running, false);
        SessionReady = true;
    }
}

bool search_is_running(void) { return SessionReady && atomic_bool_load(&Session.running); }

// Report whether a search is in flight that will not end on its own -- an infinite or
// pondering search. `unbounded` is written in search_go_start before the dispatch and
// never by the job, so reading it here off the UCI thread races nothing.
bool search_running_unbounded(void) { return search_is_running() && Session.unbounded; }

// Run thread 0's whole search: start the siblings, drive iterative deepening, join,
// vote, and emit. Dispatched onto worker 0's own OS thread so the UCI thread returns
// to read stdin -- which is what lets a `stop`/`quit`/`ponderhit` arriving mid-search
// be seen. Reads only the session and the worker set, never the caller's live board:
// the vote and the emit path go through the best worker's own root_pos copy, so a
// `position` the UCI thread might race in cannot perturb them.
static void run_main_search(void *unused) {
    (void) unused;

    SearchWorker *const w = Session.w;
    SearchManager *const sm = Session.sm;
    SearchCtx *const ctx = &w->ctx;

    // Start the siblings, then search thread 0 here. Upstream hands thread 0 a job
    // for the same reason: its `go` returns immediately, so the driver runs off the
    // input thread.
    search_threads_start_siblings(sibling_search);
    const bool uci_pv_sent = iterative_deepening(ctx, &Session.id);

    // Raise stop and collect every sibling before reading any of their root move lists.
    // The join is the happens-before edge the vote below depends on.
    thread_pool_set_stop(search_threads_pool(), true);
    search_threads_wait_siblings();

    // In `nodes as time` mode, subtract what this search spent from the budget
    // before returning (Stockfish/src/search.cpp:235-237). Read the limit here,
    // not from the snapshot taken before `search_tm_init`: that call is what
    // writes `npmsec` from the `nodestime` option, and it also scales `inc` by
    // it, so both fields only hold the values upstream subtracts once it has run.
    // Side to move comes from worker 0's own root copy, not the caller's board.
    if (ctx->limits.npmsec != 0) {
        const uint64_t total =
          PoolCounters.nodes != nullptr ? PoolCounters.nodes(PoolCounters.ctx) : ctx_nodes(ctx);
        timeman_advance_nodes_time(&sm->tm, (int64_t) total
                                              - ctx->limits.inc[board_side_to_move(&w->root_pos)]);
    }

    // Vote for the move to play. Upstream skips the vote entirely for a depth-limited or
    // skill-limited search (search.cpp), which is why `go depth N` -- and therefore the
    // bench -- always plays thread 0's move whatever the thread count.
    SearchWorker *best = w;
    if (Session.limit_depth == 0 && !Session.id.skill_enabled)
        best = search_threads_best();

    // Record what this search concluded, so the next `go` in this game seeds its
    // aspiration window and its falling-eval term from it. Upstream assigns these
    // in the driver, after the depth loop returns (search.cpp:245-246) -- which is
    // why iterative_deepening only ever reads them.
    sm->best_previous_score = best->ctx.root_moves[0].score;
    sm->best_previous_average_score = best->ctx.root_moves[0].average_score;
    sm->previous_time_reduction = Session.id.previous_time_reduction;
    // Bank the check_time counter residue so the next `go` in this game resumes
    // it, as upstream's persistent callsCnt does (thread.cpp:268 is the only
    // reset). The counter runs by value in ctx for the fast path's sake.
    sm->calls_cnt = ctx->time_state.calls_cnt;

    // Report the finished line once. The depth loop already emitted it when the last
    // MultiPV line of the final iteration completed -- but a vote that picked another
    // worker means the line on the wire is not the line being played, so re-emit.
    if (!uci_pv_sent || best != w)
        search_emit_pv(&best->ctx, best->ctx.root_depth);
    search_emit_bestmove(best->ctx.root_pos, &best->ctx.root_moves[0]);

    Session.result = result_of(&best->ctx, (TimePoint) now_ms() - Session.start);

    // Clear the guard LAST, after every session field is written: a second `go`
    // waiting on this flag must not start setup until this job is done reading and
    // writing the session.
    atomic_bool_store(&Session.running, false);
}

// Set a search up on the UCI thread and hand it off to worker 0's OS thread, then
// return. The synchronous callers (bench, the tests) reach this through search_go,
// which waits; the UCI `go` command reaches it directly and returns to the read loop.
void search_go_start(Position *pos, const SearchLimits *limits) {
    session_ensure();

    // A `go` arriving while a search is still running waits for it to finish first, as
    // upstream's ThreadPool::start_thinking does (thread.cpp). This is what lets a batch
    // of sequential `go` lines each run to completion in order, and it guards the worker
    // state the running job reads -- setting a new root over a live search would be a
    // data race. A GUI must send `stop` before a second `go` on an unbounded search, or
    // this wait does not return; that is upstream's contract too.
    search_wait();

    install_seams();

    // Prefer the clock the UCI layer stamped when it parsed `go` (upstream's start_time,
    // measured as early as possible). Fall back to stamping here when it is unset -- the
    // bench and test callers, which do not score elapsed time.
    const TimePoint start =
      limits->start_time != 0 ? (TimePoint) limits->start_time : (TimePoint) now_ms();

    thread_pool_set_stop(search_threads_pool(), false);
    thread_pool_set_increase_depth(search_threads_pool(), true);

    Session.result = (SearchResult) { .score = VALUE_ZERO, .best_move = MOVE_NONE };

    ExtMove legal[MAX_MOVES];
    const size_t count = (size_t) (generate_legal(pos, legal) - legal);

    SearchWorker *const w = search_threads_main();
    if (w == nullptr) {
        // No worker could be built, so there is nothing to search against. Return a legal
        // move rather than MOVE_NONE, as the root-move allocation failure below does.
        Session.result.best_move = count != 0 ? legal[0].move : MOVE_NONE;
        return;
    }

    SearchManager *const sm = w->manager;
    SearchCtx *const ctx = &w->ctx;

    atomic_bool_store(&sm->ponder, limits->ponder);
    sm->stop_on_ponderhit = false;
    // Do not touch sm->calls_cnt: it carries across `go` commands as upstream's
    // callsCnt does, reset only by search_manager_clear (thread.cpp:268).

    if (count == 0) {
        // Run the time-manager init and the TT generation bump before bailing out:
        // upstream orders tm.init and tt.new_search() BEFORE the rootMoves.empty()
        // check (search.cpp:204-210), so a mate/stalemate root still ages the TT.
        // Skipping the bump leaves every entry saved by later searches one
        // generation younger than upstream's, and relative_age then disagrees on
        // warm cross-position probes. The ctx fields search_tm_init reads are
        // stale from the previous go until worker_root_setup runs, so point them
        // at this go's data first; the next setup memsets ctx and rebuilds both.
        ctx->limits = to_zone_limits(limits, start);
        ctx->root_pos = pos;
        search_tm_init(ctx, &sm->tm, &sm->original_time_adjust);
        Session.result.score = board_has_checkers(pos) ? mated_in(0) : VALUE_DRAW;
        search_emit_no_moves(pos);
        return;
    }

    Move moves[MAX_MOVES];
    for (size_t i = 0; i < count; ++i)
        moves[i] = legal[i].move;

    // Hand the root FEN to the ranking: it replays each root move from that string
    // on a scratch board. Only the tablebase path reads it, which is inert here.
    char root_fen[128];
    pos_fen(pos, root_fen);

    // Rank ONCE and copy, as upstream does. The ranking is deterministic, so ranking per
    // worker would agree -- but it walks the board with do/undo for every root move, and
    // paying that N times is a cost the copy does not have.
    RootMoveList ranked;
    if (!root_moves_build(pos, root_fen, board_is_chess960(pos), moves, count, &ranked)) {
        // An allocation failure leaves nothing to search. Return a legal move
        // rather than MOVE_NONE, so the caller still has something playable.
        Session.result.best_move = legal[0].move;
        return;
    }

    const SearchZoneLimits zone_limits = to_zone_limits(limits, start);

    const size_t threads = search_threads_count();
    for (size_t i = 0; i < threads; ++i) {
        SearchWorker *const wi = search_threads_at(i);
        if (!worker_root_setup(wi, pos, root_fen, &ranked, &zone_limits)) {
            // Leave this worker with no root move list. `sibling_search` refuses such a
            // worker and the vote skips it, so one failed setup costs a thread rather
            // than the search.
            wi->ctx.root_moves = nullptr;
            wi->ctx.root_moves_count = 0;
        }
    }
    root_moves_free(&ranked);

    if (ctx->root_moves == nullptr) {
        Session.result.best_move = legal[0].move;
        return;
    }

    search_tm_init(ctx, &sm->tm, &sm->original_time_adjust);
    search_time_state_init(ctx, &sm->tm, &sm->calls_cnt, &sm->ponder.value, &sm->stop_on_ponderhit);

    search_id_state_init(&Session.id, ctx, &sm->tm, &sm->ponder.value, &sm->stop_on_ponderhit,
                         pool_increase_depth());
    Session.id.threads_size = threads;

    // Seed the per-game manager scalars search_id_state_init leaves at zero.
    Session.id.best_previous_score = sm->best_previous_score;
    Session.id.best_previous_average_score = sm->best_previous_average_score;
    Session.id.previous_time_reduction = sm->previous_time_reduction;

    Session.w = w;
    Session.sm = sm;
    Session.start = start;
    Session.threads = threads;
    Session.limit_depth = limits->depth;
    // An infinite or pondering search runs until stop/ponderhit -- it has no deadline
    // of its own, so `quit` must raise stop to end it. Every other search terminates by
    // depth, nodes, or the clock, and `quit` waits it out instead (a bounded search's
    // output stays deterministic, which the golden gate relies on).
    Session.unbounded = limits->infinite || limits->ponder;

    // Arm the guard, then dispatch onto thread 0. Everything above ran on the UCI
    // thread; from here the search runs on worker 0's OS thread and this call returns.
    atomic_bool_store(&Session.running, true);
    thread_pool_run_on_thread(search_threads_pool(), 0, run_main_search, nullptr);
}

// Block until the in-flight search, if any, has finished and published its result.
// A no-op when nothing is running (no dispatched job, or the synchronous early-outs
// in search_go_start that never reach thread 0).
void search_wait(void) { thread_pool_wait_on_thread(search_threads_pool(), 0); }

// Clear the ponder flag worker 0's check_time polls, so a `go ponder` search that was
// exempt from every time limit begins enforcing them (upstream: SearchManager::ponder
// = false on `ponderhit`). The clock still counts from the `go` parse, so the search
// stops no later than a non-pondered one would -- never past its budget.
void search_ponderhit(void) {
    if (!search_is_running())
        return;
    SearchWorker *const w = search_threads_main();
    if (w != nullptr && w->manager != nullptr)
        atomic_bool_store(&w->manager->ponder, false);
}

SearchResult search_go(Position *pos, const SearchLimits *limits) {
    search_go_start(pos, limits);
    search_wait();
    return Session.result;
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
            pos_do_move(pos, list[i].move, &st, pos_gives_check(pos, list[i].move),
                        &pos->scratch_dp, &pos->scratch_dts);
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
