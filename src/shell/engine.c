#include "engine.h"

#include "../engine/board/types.h"
#include "../engine/board/uci_move.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/tt.h"
#include "misc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_GAME_PLIES = 1024 };

// Keep the whole state chain alive for the game: pos_undo_move and the
// repetition scan both follow StateInfo::previous, so a state popped off the C
// stack would leave the chain pointing at freed memory.
static Position Pos;
static StateInfo States[MAX_GAME_PLIES];
static int StatesUsed = 0;

static OptionsMap Options;
static void (*Emit)(const char *line) = nullptr;
static bool Initialized = false;

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void emit(const char *line) {
    if (Emit)
        Emit(line);
}

void engine_set_output(void (*sink)(const char *line)) {
    Emit = sink;
    search_set_output(sink);
    options_set_info(&Options, sink ? emit : nullptr);
}

// ---------------------------------------------------------------------------
// Option on-change callbacks
//
// Each returns a message for the info listener, or nullptr for silence, matching
// upstream's `std::optional<std::string>` OnChange. A callback whose subsystem is
// unported says so on the wire rather than accepting the value silently — a GUI
// that gets no answer cannot tell a no-op from a working feature.
// ---------------------------------------------------------------------------

static char MessageBuf[256];

static const char *on_hash(const UciOption *o) {
    const size_t mb = (size_t) strtoul(o->current_value, nullptr, 10);
    if (!tt_resize(mb)) {
        snprintf(MessageBuf, sizeof MessageBuf, "info string failed to allocate %zu MB hash", mb);
        return MessageBuf;
    }
    return nullptr;
}

static const char *on_clear_hash(const UciOption *o) {
    (void) o;
    engine_search_clear();
    return nullptr;
}

// NO-OP: the thread pool is unported (M4 — upstream `thread.cpp`).
//
// This message is unreachable while MAX_THREADS is 1: the spin bounds reject any
// other value before the callback runs, so `setoption name Threads value 4` is
// refused outright rather than accepted-and-ignored. That is the stronger
// answer, and it is what changes when MAX_THREADS rises. Keep the callback — it
// is the seam that keeps the advertisement honest for any interval between
// raising the maximum and the pool actually landing.
static const char *on_threads(const UciOption *o) {
    if (strcmp(o->current_value, "1") != 0)
        return "info string Threads is accepted but the search is single-threaded";
    return nullptr;
}

// NO-OP: the debug logger is unported (upstream `misc.cpp` start_logger).
static const char *on_debug_log_file(const UciOption *o) {
    if (o->current_value[0])
        return "info string Debug Log File is accepted but logging is not implemented";
    return nullptr;
}

// NO-OP: NUMA topology and replication are unported (M4 — upstream `numa.h`).
static const char *on_numa_policy(const UciOption *o) {
    (void) o;
    return "info string NumaPolicy is accepted but NUMA binding is not implemented";
}

// NO-OP: Syzygy probing is unported (M5 — upstream `syzygy/tbprobe.cpp`).
// Report only a non-empty path: the default is empty and a GUI clearing the
// option should not be nagged.
static const char *on_syzygy_path(const UciOption *o) {
    if (o->current_value[0])
        return "info string SyzygyPath is accepted but tablebase probing is not implemented";
    return nullptr;
}

// NO-OP: NNUE is unported (M3 — upstream `nnue/network.cpp`). The evaluation in
// src/engine/eval/evaluate.c is a classical placeholder scheduled for deletion
// and loads no net file, so nothing reads this path yet.
static const char *on_eval_file(const UciOption *o) {
    (void) o;
    return "info string EvalFile is accepted but NNUE is not implemented";
}

// ---------------------------------------------------------------------------
// Option registration
//
// The order below IS the wire order, and it is upstream's registration order in
// engine.cpp:69 onward, not the enum order or an alphabetical one. A GUI reads
// the handshake in this sequence and tools/handshake.golden diffs it byte for
// byte, so do not reorder to group related options together.
// ---------------------------------------------------------------------------

// Skill's Elo window, from upstream `search.h:243`. Nothing consumes it yet:
// the skill-limited move selection lands with the search port (M2).
enum { SKILL_LOWEST_ELO = 1320, SKILL_HIGHEST_ELO = 3190 };

// Advertise a maximum of 1, not upstream's `max(1024, 4 * hardware_concurrency)`.
// The search is single-threaded, and an advertised maximum a GUI can act on is a
// claim the engine cannot honour. This becomes upstream's expression at M4, when
// the thread pool lands; do not raise it ahead of the pool.
enum { MAX_THREADS = 1 };

// Upstream's `MaxHashMB = Is64Bit ? 33554432 : 2048` (engine.cpp:51).
static int max_hash_mb(void) { return sizeof(size_t) >= 8 ? 33554432 : 2048; }

// Upstream's EvalFileDefaultName (evaluate.h:36). Kept at upstream's value so
// the option is correct the day NNUE lands; the on-change callback above states
// plainly that nothing loads it today.
#define EVAL_FILE_DEFAULT_NAME "nn-0ee0657fb25e.nnue"

static void register_options(void) {
    char buf[16];

    options_clear(&Options);

    options_add(&Options, "Debug Log File", OPTION_STRING, "", 0, 0, on_debug_log_file);
    options_add(&Options, "NumaPolicy", OPTION_STRING, "auto", 0, 0, on_numa_policy);
    options_add(&Options, "Threads", OPTION_SPIN, "1", 1, MAX_THREADS, on_threads);
    options_add(&Options, "Hash", OPTION_SPIN, "16", 1, max_hash_mb(), on_hash);
    options_add(&Options, "Clear Hash", OPTION_BUTTON, "", 0, 0, on_clear_hash);
    options_add(&Options, "Ponder", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "MultiPV", OPTION_SPIN, "1", 1, MAX_MOVES, nullptr);
    options_add(&Options, "Skill Level", OPTION_SPIN, "20", 0, 20, nullptr);
    options_add(&Options, "Move Overhead", OPTION_SPIN, "10", 0, 5000, nullptr);
    options_add(&Options, "nodestime", OPTION_SPIN, "0", 0, 10000, nullptr);
    options_add(&Options, "UCI_Chess960", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "UCI_LimitStrength", OPTION_CHECK, "false", 0, 0, nullptr);

    snprintf(buf, sizeof buf, "%d", SKILL_LOWEST_ELO);
    options_add(&Options, "UCI_Elo", OPTION_SPIN, buf, SKILL_LOWEST_ELO, SKILL_HIGHEST_ELO,
                nullptr);

    options_add(&Options, "UCI_ShowWDL", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "SyzygyPath", OPTION_STRING, "", 0, 0, on_syzygy_path);
    options_add(&Options, "SyzygyProbeDepth", OPTION_SPIN, "1", 1, 100, nullptr);
    options_add(&Options, "Syzygy50MoveRule", OPTION_CHECK, "true", 0, 0, nullptr);
    options_add(&Options, "SyzygyProbeLimit", OPTION_SPIN, "7", 0, 7, nullptr);
    options_add(&Options, "EvalFile", OPTION_STRING, EVAL_FILE_DEFAULT_NAME, 0, 0, on_eval_file);
}

OptionsMap *engine_get_options(void) { return &Options; }

Position *engine_get_position(void) { return &Pos; }

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void engine_init(void) {
    register_options();
    options_set_info(&Options, Emit ? emit : nullptr);

    // Size the table from the registered default rather than a second literal,
    // so `Hash`'s default and the table's initial size cannot drift apart.
    engine_set_tt_size((size_t) options_get_int(&Options, "Hash"));
    engine_set_position(ENGINE_START_FEN);
    Initialized = true;
}

void engine_shutdown(void) {
    if (!Initialized)
        return;
    tt_free();
    Initialized = false;
}

bool engine_set_position(const char *fen) {
    const bool chess960 = options_get_int(&Options, "UCI_Chess960") != 0;

    StatesUsed = 0;
    if (pos_set(&Pos, fen, chess960, &States[StatesUsed++]))
        return true;

    // Fall back rather than leave Pos unspecified: king_square would lsb an
    // empty bitboard, which is undefined behaviour, not a diagnosable error.
    StatesUsed = 0;
    pos_set(&Pos, ENGINE_START_FEN, chess960, &States[StatesUsed++]);
    return false;
}

bool engine_play_move(const char *uci_move) {
    if (StatesUsed >= MAX_GAME_PLIES)
        return false;

    const Move m = move_from_uci(&Pos, uci_move);
    if (m == MOVE_NONE)
        return false;

    pos_do_move(&Pos, m, &States[StatesUsed++], false, &Pos.scratch_dp, &Pos.scratch_dts);
    return true;
}

SearchResult engine_go(const SearchLimits *limits) { return search_go(&Pos, limits); }

uint64_t engine_perft(int depth) { return perft(&Pos, depth, true); }

void engine_stop(void) { search_stop(); }

void engine_search_clear(void) {
    // Upstream also waits for the search to finish, clears the per-thread
    // histories, and re-inits the tablebases here (engine.cpp Engine::search_clear).
    // The search is single-threaded and synchronous and both subsystems are
    // unported, so clearing the table is the whole of it today.
    tt_clear();
}

void engine_set_tt_size(size_t mb) {
    if (!tt_resize(mb)) {
        snprintf(MessageBuf, sizeof MessageBuf, "info string failed to allocate %zu MB hash", mb);
        emit(MessageBuf);
    }
}

int engine_get_hashfull(void) { return tt_hashfull(0); }

void engine_visualize(char *buf, int buf_len) { pos_pretty(&Pos, buf, buf_len); }

void engine_trace_eval(char *buf, int buf_len) { evaluate_trace(&Pos, buf, buf_len); }
