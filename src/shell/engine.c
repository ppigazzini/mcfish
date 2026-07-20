// Implement the engine session. See engine.h for the seam. This is the live
// behaviour lifted out of the old uci.c monolith: the wired option callbacks
// (Hash, Threads, NumaPolicy, the four Syzygy options, EvalFile), the unbounded
// state chain, the net load, and the search wiring. uci.c now only parses text,
// prints text, and drives the calls here.

#include "engine.h"

#include "../engine/board/board_props.h"
#include "../engine/board/movegen.h"
#include "../engine/board/state_list.h"
#include "../engine/board/uci_move.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/tt.h"
#include "syzygy_option.h"
#include "uci.h"
#include "ucioption.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENGINE_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// Keep the whole state chain alive for the game: pos_undo_move and the repetition
// scan both follow StateInfo::previous, so a state popped off the C stack would
// leave the chain pointing at freed memory. Each record is its own allocation, so
// a push never moves one already handed out. Upstream's chain is a deque with no
// bound (engine.cpp:210).
static Position Pos;
static StateList *States = nullptr;
static OptionsMap Options;

// The directory the binary was launched from, with its trailing separator, as the
// third candidate network_load searches (after "<internal>" and the cwd).
static char RootDirectory[512];

static bool NetOk = false;
static char MessageBuf[256];
static char ReasonBuf[128];

// The two line sinks the session emits through, installed by engine_set_output.
static void (*EmitLine)(const char *line) = nullptr;
static void (*EmitInfo)(const char *message) = nullptr;

void engine_set_output(void (*emit_line)(const char *line),
                       void (*emit_info)(const char *message)) {
    EmitLine = emit_line;
    EmitInfo = emit_info;
    search_set_output(emit_line);
}

// ---------------------------------------------------------------------------
// Net
// ---------------------------------------------------------------------------

// Load the net named by EvalFile. Silent: engine_report_net announces the outcome,
// so a load and a re-load read the same on the wire.
static void load_net(void) {
    NetOk = eval_nnue_load(RootDirectory, options_get_string(&Options, "EvalFile"));
}

void engine_report_net(void) {
    if (!EmitLine)
        return;
    char line[512];
    snprintf(line, sizeof line, "info string %s", eval_nnue_status());
    EmitLine(line);
}

// Refuse to run without a usable net, as upstream does (nnue/network.cpp:165-187,
// reached from `go`, `perft` and `eval`). The message is upstream's five lines
// verbatim, including the file name and the download URL.
void engine_verify_network(void) {
    if (NetOk)
        return;

    const char *const want = options_get_string(&Options, "EvalFile");
    fprintf(stderr,
            "ERROR: Network evaluation parameters compatible with the engine must be "
            "available.\n"
            "ERROR: The network file %s was not loaded successfully.\n"
            "ERROR: The UCI option EvalFile might need to specify the full path, including "
            "the directory name, to the network file.\n"
            "ERROR: The default net can be downloaded from: "
            "https://tests.stockfishchess.org/api/nn/%s\n"
            "ERROR: The engine will be terminated now.\n",
            want, eval_nnue_default_file_name());
    exit(EXIT_FAILURE);
}

// ---------------------------------------------------------------------------
// Option on-change callbacks
//
// Each returns bare text for the info listener, or nullptr for silence, matching
// upstream's std::optional<std::string> OnChange.
// ---------------------------------------------------------------------------

static const char *on_hash(const UciOption *o) {
    const size_t mb = (size_t) strtoul(o->current_value, nullptr, 10);
    if (!tt_resize(mb)) {
        snprintf(MessageBuf, sizeof MessageBuf, "failed to allocate %zu MB hash", mb);
        return MessageBuf;
    }
    return nullptr;
}

// Clear the table AND the per-game state, as upstream's Engine::search_clear does
// (engine.cpp:172): the button and `ucinewgame` reach the same function.
static const char *on_clear_hash(const UciOption *o) {
    (void) o;
    tt_clear();
    search_clear();
    return nullptr;
}

static const char *on_debug_log_file(const UciOption *o) {
    uci_start_logger(o->current_value);
    return nullptr;
}

// Rebuild the worker set. Upstream reaches ThreadPool::set from the same option and
// rebuilds rather than resizes, because a thread must be created on the NUMA node it
// will run on. Owner: upstream `thread.cpp` (ThreadPool::set).
static const char *on_threads(const UciOption *o) {
    const size_t n = (size_t) strtoul(o->current_value, nullptr, 10);
    if (!search_set_threads(n)) {
        snprintf(MessageBuf, sizeof MessageBuf, "failed to build %zu search thread(s)", n);
        return MessageBuf;
    }
    return nullptr;
}

// Install the NUMA topology the next pool binds under, then re-apply the thread
// count so the policy takes effect now. Owner: upstream `numa.h`, engine.cpp:227.
static const char *on_numa_policy(const UciOption *o) {
    if (!search_set_numa_policy(o->current_value)) {
        snprintf(MessageBuf, sizeof MessageBuf, "NumaPolicy \"%s\" names no usable node",
                 o->current_value);
        return MessageBuf;
    }
    (void) search_set_threads((size_t) options_get_int(&Options, "Threads"));
    return nullptr;
}

// Hand a Syzygy option to the module that owns the four of them and the tablebase
// seams. The table has range-checked the value already. Golden: engine.cpp:125-134.
static const char *on_syzygy(const UciOption *o) {
    (void) syzygy_option_set(o->name, o->current_value);
    return nullptr;
}

static const char *on_eval_file(const UciOption *o) {
    (void) o;
    load_net();
    // Drop the search state the previous net produced, as upstream follows every
    // load with threads.clear() (engine.cpp:313). The resident net is announced by
    // engine_report_net on the next go/perft/eval, so report nothing here.
    search_clear();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Option registration
//
// The order below IS the wire order, upstream's registration order in engine.cpp:69
// onward. tools/handshake.golden diffs it byte for byte, so do not reorder.
// ---------------------------------------------------------------------------

enum { SKILL_LOWEST_ELO = 1320, SKILL_HIGHEST_ELO = 3190 };

// Upstream's max(1024, 4 * get_hardware_concurrency()) (engine.cpp:52), ported as
// the expression so the advertised maximum tracks upstream past 256 cores.
static int max_threads(void) {
    long online = 0;
#if defined(_SC_NPROCESSORS_ONLN)
    online = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    const long scaled = online > 0 ? 4 * online : 0;
    return scaled > 1024 ? (int) scaled : 1024;
}

// Upstream's MaxHashMB = Is64Bit ? 33554432 : 2048 (engine.cpp:51).
static int max_hash_mb(void) { return sizeof(size_t) >= 8 ? 33554432 : 2048; }

// Read a spin or check option for the search zone, so MultiPV, Skill Level, Move
// Overhead, nodestime, Ponder, UCI_Elo/LimitStrength and UCI_ShowWDL reach the
// search from the same table the handshake renders.
static int option_int_for_search(const char *name) { return options_get_int(&Options, name); }

static void register_options(void) {
    char elo[16];

    options_clear(&Options);
    options_set_info(&Options, EmitInfo);

    options_add(&Options, "Debug Log File", OPTION_STRING, "", 0, 0, on_debug_log_file);
    options_add(&Options, "NumaPolicy", OPTION_STRING, "auto", 0, 0, on_numa_policy);
    options_add(&Options, "Threads", OPTION_SPIN, "1", 1, max_threads(), on_threads);
    options_add(&Options, "Hash", OPTION_SPIN, "16", 1, max_hash_mb(), on_hash);
    options_add(&Options, "Clear Hash", OPTION_BUTTON, "", 0, 0, on_clear_hash);
    options_add(&Options, "Ponder", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "MultiPV", OPTION_SPIN, "1", 1, MAX_MOVES, nullptr);
    options_add(&Options, "Skill Level", OPTION_SPIN, "20", 0, 20, nullptr);
    options_add(&Options, "Move Overhead", OPTION_SPIN, "10", 0, 5000, nullptr);
    options_add(&Options, "nodestime", OPTION_SPIN, "0", 0, 10000, nullptr);
    options_add(&Options, "UCI_Chess960", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "UCI_LimitStrength", OPTION_CHECK, "false", 0, 0, nullptr);

    snprintf(elo, sizeof elo, "%d", SKILL_LOWEST_ELO);
    options_add(&Options, "UCI_Elo", OPTION_SPIN, elo, SKILL_LOWEST_ELO, SKILL_HIGHEST_ELO,
                nullptr);

    options_add(&Options, "UCI_ShowWDL", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "SyzygyPath", OPTION_STRING, "", 0, 0, on_syzygy);
    options_add(&Options, "SyzygyProbeDepth", OPTION_SPIN, "1", 1, 100, on_syzygy);
    options_add(&Options, "Syzygy50MoveRule", OPTION_CHECK, "true", 0, 0, on_syzygy);
    options_add(&Options, "SyzygyProbeLimit", OPTION_SPIN, "7", 0, 7, on_syzygy);
    options_add(&Options, "EvalFile", OPTION_STRING, eval_nnue_default_file_name(), 0, 0,
                on_eval_file);
}

OptionsMap *engine_options(void) { return &Options; }

Position *engine_position(void) { return &Pos; }

int engine_setoption(const char *args, char name[OPTION_NAME_MAX]) {
    return options_setoption(&Options, args, name);
}

void engine_render_options(char *buf, size_t buf_len) {
    (void) buf_len;
    options_render(&Options, buf, buf_len);
}

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

bool engine_set_position_variant(const char *fen, bool chess960, const char **reason) {
    const char *r = nullptr;
    StateInfo *const root = state_list_reset(States);
    if (pos_set_reason(&Pos, fen, chess960, root, &r))
        return true;
    if (reason)
        *reason = r ? r : "Invalid FEN.";
    return false;
}

bool engine_set_position(const char *fen, const char **reason) {
    return engine_set_position_variant(fen, options_get_int(&Options, "UCI_Chess960") != 0, reason);
}

bool engine_set_startpos(const char **reason) {
    return engine_set_position(ENGINE_START_FEN, reason);
}

// Re-set the board from the colour-reversed form of its own FEN. The variant comes
// from the BOARD, not the live option: upstream ends Position::flip with
// set(f, is_chess960(), st) (position.cpp:1633).
void engine_flip(const char **reason) {
    char fen[128];
    pos_fen(&Pos, fen);

    char flipped[128];
    if (!pos_flip_fen(fen, flipped))
        return;

    (void) engine_set_position_variant(flipped, board_is_chess960(&Pos), reason);
}

bool engine_play_move(const char *uci_move, const char **reason) {
    const Move m = move_from_uci(&Pos, uci_move);
    if (m == MOVE_NONE) {
        snprintf(ReasonBuf, sizeof ReasonBuf, "Illegal move: %s", uci_move);
        if (reason)
            *reason = ReasonBuf;
        return false;
    }
    StateInfo *const st = state_list_push(States);
    if (st == nullptr) {
        if (reason)
            *reason = "Out of memory extending the state chain.";
        return false;
    }
    pos_do_move(&Pos, m, st, false, &Pos.scratch_dp, &Pos.scratch_dts);
    return true;
}

void engine_new_game(void) {
    tt_clear();
    search_clear();
    const char *r = nullptr;
    (void) engine_set_startpos(&r);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

void engine_go(const SearchLimits *limits) {
    // The search zone emits `bestmove` itself, through the installed sink, so the
    // line is built once and in one place. Do not print a second one.
    (void) search_go(&Pos, limits);
}

uint64_t engine_perft(int depth) { return perft(&Pos, depth, true); }

void engine_stop(void) { search_stop(); }

void engine_current_fen(char *buf, size_t buf_len) {
    char fen[128];
    pos_fen(&Pos, fen);
    snprintf(buf, buf_len, "%s", fen);
}

void engine_visualize(char *buf, int buf_len) { pos_pretty(&Pos, buf, buf_len); }

void engine_trace_eval(char *buf, int buf_len) { evaluate_trace(&Pos, buf, buf_len); }

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

// Record the directory ARGV0 was launched from, with its trailing separator. Leave
// it empty when argv[0] carries no separator: the cwd is already candidate two.
static void set_root_directory(const char *argv0) {
    RootDirectory[0] = '\0';
    if (argv0 == nullptr)
        return;
    const char *slash = strrchr(argv0, '/');
    if (slash == nullptr)
        return;
    const size_t len = (size_t) (slash - argv0) + 1;
    if (len >= sizeof RootDirectory)
        return;
    memcpy(RootDirectory, argv0, len);
    RootDirectory[len] = '\0';
}

void engine_init(const char *argv0) {
    // Build the state chain before anything can set a position.
    States = state_list_create();
    if (States == nullptr) {
        fprintf(stderr, "Out of memory allocating the state chain\n");
        exit(EXIT_FAILURE);
    }

    // Bind the tablebase seams before the first search: until this runs the engine
    // reads the neutral defaults, which never probe.
    syzygy_option_install();

    register_options();

    // Clear the search state before the first command, as upstream does from the
    // Engine constructor (engine.cpp:145): the histories are NOT zero when clear, so
    // an engine that skips this searches a different tree until the first ucinewgame.
    search_clear();

    // Point the search at this table so MultiPV, Skill Level, UCI_Elo, Move
    // Overhead, nodestime, Ponder and UCI_ShowWDL are read where the handshake
    // advertises them.
    search_set_option_source(option_int_for_search);

    // Size the table from the registered default rather than a second literal.
    tt_resize((size_t) options_get_int(&Options, "Hash"));

    const char *r = nullptr;
    (void) engine_set_startpos(&r);

    // Load the net before the first command: go/perft/eval all report the outcome.
    set_root_directory(argv0);
    load_net();
}

void engine_shutdown(void) { tt_free(); }
