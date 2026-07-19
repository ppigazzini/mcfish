#include "uci.h"

#include "../engine/board/movegen.h"
#include "../engine/board/position.h"
#include "../engine/board/uci_move.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../engine/search/tt.h"
#include "benchmark.h"
#include "ucioption.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENGINE_NAME "ccfish"
#define ENGINE_VERSION "dev"
#define ENGINE_AUTHORS "the Stockfish developers (see AUTHORS file)"

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

enum { MAX_GAME_PLIES = 1024 };

// Keep the whole state chain alive for the game: pos_undo_move and the repetition
// scan both follow StateInfo::previous, so a state popped off the C stack would
// leave the chain pointing at freed memory.
static Position Pos;
static StateInfo States[MAX_GAME_PLIES];
static int StatesUsed = 0;

static OptionsMap Options;

// Hold the directory the binary was launched from. The net is a runtime input,
// never embedded, and network_load searches "<internal>", the working directory,
// then this root — which must carry its trailing separator, because the
// concatenation inserts none.
static char RootDirectory[512];

// ---------------------------------------------------------------------------
// Output funnel and the debug log
//
// Every byte the engine writes to a GUI leaves through uci_write. That is what
// makes `Debug Log File` implementable at all: upstream ties std::cout and
// std::cin to a tee streambuf (misc.cpp, struct Tie), so the log holds the whole
// session with each output line prefixed "<< " and each input line ">> ". With
// scattered printf calls there is no single point to tee, and the log would
// silently hold a subset of the session — which is worse than no log, because it
// reads as a complete transcript.
// ---------------------------------------------------------------------------

static FILE *LogFile = nullptr;

// Track the last byte written to the log so a prefix is emitted exactly at line
// starts, across the input/output boundary. Upstream shares one `last` between
// the cin and cout ties (misc.cpp, Tie::log) and never resets it on reopen; keep
// both properties, or a reopened log gains a stray prefix mid-line.
static int LogLast = '\n';

static void log_bytes(const char *s, size_t n, const char *prefix) {
    if (!LogFile)
        return;
    for (size_t i = 0; i < n; ++i) {
        if (LogLast == '\n')
            fwrite(prefix, 1, 3, LogFile);
        fputc(s[i], LogFile);
        LogLast = (unsigned char) s[i];
    }
    fflush(LogFile);
}

// Write S verbatim — no newline is appended, so a caller composing a multi-line
// block controls its own line breaks.
static void uci_write(const char *s) {
    const size_t n = strlen(s);
    fwrite(s, 1, n, stdout);
    fflush(stdout);
    log_bytes(s, n, "<< ");
}

[[gnu::format(printf, 1, 2)]]
static void uci_printf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    // vsnprintf reports what it WOULD have written; clamp to what it did.
    const size_t len = (size_t) n < sizeof buf ? (size_t) n : sizeof buf - 1;
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    log_bytes(buf, len, "<< ");
}

// Open FNAME as the session log, closing any previous one. Exit on a path that
// cannot be opened, as upstream does (misc.cpp, Logger::start): an operator who
// asked for a transcript and silently got none cannot tell the run apart from
// one where nothing happened.
static void start_logger(const char *fname) {
    if (LogFile) {
        fclose(LogFile);
        LogFile = nullptr;
    }
    if (!fname[0])
        return;

    LogFile = fopen(fname, "w");
    if (!LogFile) {
        fprintf(stderr, "Unable to open debug log file %s\n", fname);
        exit(EXIT_FAILURE);
    }
}

// Report the resident net, or why the classical fallback is in use. Upstream
// prints this before every go, perft and eval (engine.cpp:150, :157, :329) and
// terminates when no net loaded; ccfish keeps playing on the fallback instead, so
// the same three sites print and none exits.
static void report_net(void) { uci_printf("info string %s\n", eval_nnue_status()); }

// Write the search's line and its terminator without going through uci_printf: a
// PV info line is built in a 5120-byte buffer (search_emit.c LINE_MAX) and would
// be silently truncated by the smaller printf staging buffer.
static void emit_stdout(const char *line) {
    uci_write(line);
    uci_write("\n");
}

// Route an on-change message, one "info string" line per line of text, as
// upstream's UCIEngine::print_info_string does (uci.cpp:55). The callbacks return
// bare text for exactly this reason: the prefix belongs to the transport.
static void emit_info_string(const char *message) {
    const char *line = message;
    while (*line) {
        const char *end = strchr(line, '\n');
        const int len = end ? (int) (end - line) : (int) strlen(line);
        if (len > 0)
            uci_printf("info string %.*s\n", len, line);
        if (!end)
            break;
        line = end + 1;
    }
}

// ---------------------------------------------------------------------------
// Option on-change callbacks
//
// Each returns bare text for the info listener, or nullptr for silence, matching
// upstream's `std::optional<std::string>` OnChange. A callback whose subsystem is
// unported says so on the wire rather than accepting the value silently — a GUI
// that gets no answer cannot tell a no-op from a working feature.
// ---------------------------------------------------------------------------

static char MessageBuf[256];

// Load the net named by the EvalFile option. Silent: a caller reports the outcome
// through report_net, so a load and a re-load read the same on the wire.
static void load_net(void) {
    (void) eval_nnue_load(RootDirectory, options_get_string(&Options, "EvalFile"));
}

static const char *on_hash(const UciOption *o) {
    const size_t mb = (size_t) strtoul(o->current_value, nullptr, 10);
    if (!tt_resize(mb)) {
        snprintf(MessageBuf, sizeof MessageBuf, "failed to allocate %zu MB hash", mb);
        return MessageBuf;
    }
    return nullptr;
}

// Clear the table AND the per-game state, as upstream's Engine::search_clear does
// (engine.cpp:172). The button and `ucinewgame` reach the same function upstream,
// so clearing only the TT here would leave a history block the next search reads.
static const char *on_clear_hash(const UciOption *o) {
    (void) o;
    tt_clear();
    search_clear();
    return nullptr;
}

static const char *on_debug_log_file(const UciOption *o) {
    start_logger(o->current_value);
    return nullptr;
}

// ADVERTISED BUT INERT: the thread pool is unported, so the search runs on one
// thread whatever this says. Upstream's maximum is advertised anyway because a
// narrower one is a different handshake, and the handshake is what a GUI
// configures against. Any value above 1 is accepted and ignored; say so, because
// a GUI that sets Threads 8 and sees silence has no way to learn otherwise.
// Owner: zfish `platform/thread_pool.zig`, upstream `thread.cpp`.
static const char *on_threads(const UciOption *o) {
    if (strcmp(o->current_value, "1") != 0)
        return "Threads is accepted but ignored: the search is single-threaded";
    return nullptr;
}

// ADVERTISED BUT INERT: NUMA topology discovery and thread binding are unported.
// Upstream answers with the resulting node/thread layout; there is no layout to
// report. Owner: upstream `numa.h`.
static const char *on_numa_policy(const UciOption *o) {
    (void) o;
    return "NumaPolicy is accepted but ignored: NUMA binding is not implemented";
}

// ADVERTISED BUT INERT: Syzygy probing is unported, so no path is opened and no
// tablebase is consulted. Report only a non-empty path: the default is empty and
// a GUI clearing the option should not be nagged. Owner: zfish
// `engine/syzygy/`, upstream `syzygy/tbprobe.cpp`.
static const char *on_syzygy_path(const UciOption *o) {
    if (o->current_value[0])
        return "SyzygyPath is accepted but ignored: tablebase probing is not implemented";
    return nullptr;
}

static const char *on_eval_file(const UciOption *o) {
    (void) o;
    load_net();
    return eval_nnue_status();
}

// ---------------------------------------------------------------------------
// Option registration
//
// The order below IS the wire order, and it is upstream's registration order in
// engine.cpp:69 onward, not the enum order or an alphabetical one. A GUI reads the
// handshake in this sequence and tools/handshake.golden diffs it byte for byte, so
// do not reorder to group related options together.
// ---------------------------------------------------------------------------

// Skill's Elo window, from upstream `search.h` (Skill::LowestElo / HighestElo).
enum { SKILL_LOWEST_ELO = 1320, SKILL_HIGHEST_ELO = 3190 };

// Upstream's `MaxThreads = std::max(1024, 4 * get_hardware_concurrency())`
// (engine.cpp:52). Ported as the expression, not as the constant it happens to
// evaluate to here, so the advertised maximum tracks upstream on a machine with
// more than 256 cores — where the two would otherwise disagree.
static int max_threads(void) {
    long online = 0;
#if defined(_SC_NPROCESSORS_ONLN)
    online = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    const long scaled = online > 0 ? 4 * online : 0;
    return scaled > 1024 ? (int) scaled : 1024;
}

// Upstream's `MaxHashMB = Is64Bit ? 33554432 : 2048` (engine.cpp:51).
static int max_hash_mb(void) { return sizeof(size_t) >= 8 ? 33554432 : 2048; }

// Read a spin or check option for the search zone. Registered through
// search_set_option_source so MultiPV, Skill Level, UCI_LimitStrength, UCI_Elo,
// Move Overhead, nodestime, Ponder and UCI_ShowWDL reach the search from the same
// table the handshake renders — one source, so what a GUI sets is what runs.
static int option_int_for_search(const char *name) { return options_get_int(&Options, name); }

static void register_options(void) {
    char elo[16];

    options_clear(&Options);
    options_set_info(&Options, emit_info_string);

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

    // ADVERTISED BUT INERT, all four: nothing registers the TbProbeFen /
    // TbMaxCardinality seams in src/engine/search/tb_source.h, so the root ranker
    // reads a zero cardinality and never probes. The values below are stored and
    // rendered but are deliberately NOT fed to OptionSyzygyProbeDepth /
    // OptionSyzygyProbeLimit / OptionSyzygy50MoveRule — feeding a prober-less
    // search a probe budget only moves the no-op somewhere harder to see.
    options_add(&Options, "SyzygyPath", OPTION_STRING, "", 0, 0, on_syzygy_path);
    options_add(&Options, "SyzygyProbeDepth", OPTION_SPIN, "1", 1, 100, nullptr);
    options_add(&Options, "Syzygy50MoveRule", OPTION_CHECK, "true", 0, 0, nullptr);
    options_add(&Options, "SyzygyProbeLimit", OPTION_SPIN, "7", 0, 7, nullptr);

    options_add(&Options, "EvalFile", OPTION_STRING, eval_nnue_default_file_name(), 0, 0,
                on_eval_file);
}

// Report an invalid command and stop the process, as upstream does
// (Stockfish/src/uci.cpp:684). Terminating IS the contract: a GUI that sent a
// position the engine could not set must not receive a `bestmove` about some other
// board. ccfish previously reset to the start position and carried on answering,
// which is worse than an error -- it is a confident answer to a question nobody
// asked, and two goldens were generated over it and pinned it.
static void terminate_on_critical_error(const char *command, const char *reason) {
    // Two newlines again: upstream writes '\n' and then sync_endl (uci.cpp:685-687).
    uci_printf("info string CRITICAL ERROR: Command `%s` failed. Reason: %s\n\n", command, reason);
    exit(1);
}

// Hold the command being executed, so the diagnostic can quote it verbatim the way
// upstream's `currentCmd` does.
static char CurrentCmd[4096];

// Reset the position and the state chain together. Any path that sets a new
// position must come through here, or States accumulates across games.
static void set_position(const char *fen) {
    const char *reason = nullptr;
    const bool chess960 = options_get_int(&Options, "UCI_Chess960") != 0;
    StatesUsed = 0;
    if (!pos_set_reason(&Pos, fen, chess960, &States[StatesUsed++], &reason))
        terminate_on_critical_error(CurrentCmd, reason ? reason : "Invalid FEN.");
}

static void cmd_position(char *args) {
    char *token = strtok(args, " \t\n");
    if (!token)
        return;

    if (strcmp(token, "startpos") == 0) {
        set_position(START_FEN);
        token = strtok(nullptr, " \t\n");
    } else if (strcmp(token, "fen") == 0) {
        // Reassemble the FEN: it is six space-separated fields, so it cannot be
        // read as one token, and it ends at `moves` or at end of line.
        char fen[128] = { 0 };
        int n = 0;
        while ((token = strtok(nullptr, " \t\n")) && strcmp(token, "moves") != 0) {
            const int len = (int) strlen(token);
            if (n + len + 2 >= (int) sizeof fen)
                break;
            if (n)
                fen[n++] = ' ';
            memcpy(fen + n, token, (size_t) len);
            n += len;
        }
        set_position(fen);
    } else {
        return;
    }

    if (token && strcmp(token, "moves") != 0)
        token = strtok(nullptr, " \t\n");

    if (token && strcmp(token, "moves") == 0)
        while ((token = strtok(nullptr, " \t\n"))) {
            const Move m = move_from_uci(&Pos, token);
            // An unrecognised move is a failed command, not a place to stop reading.
            // Upstream's message names the move, and this text is compared against it.
            if (m == MOVE_NONE) {
                char msg[64];
                snprintf(msg, sizeof msg, "Illegal move: %s", token);
                terminate_on_critical_error(CurrentCmd, msg);
            }
            if (StatesUsed >= MAX_GAME_PLIES)
                break;
            pos_do_move(&Pos, m, &States[StatesUsed++], false, &Pos.scratch_dp, &Pos.scratch_dts);
        }
}

static void cmd_go(char *args) {
    SearchLimits limits = { 0 };

    for (char *token = strtok(args, " \t\n"); token; token = strtok(nullptr, " \t\n")) {
        char *value = strtok(nullptr, " \t\n");

        if (strcmp(token, "infinite") == 0) {
            limits.infinite = true;
            if (value)
                token = value;  // `infinite` takes no argument; do not swallow the next one
            continue;
        }
        if (strcmp(token, "ponder") == 0) {
            limits.ponder = true;
            continue;
        }
        if (!value)
            break;

        const long v = strtol(value, nullptr, 10);
        if (strcmp(token, "depth") == 0)
            limits.depth = (int) v;
        else if (strcmp(token, "movetime") == 0)
            limits.movetime_ms = (int) v;
        else if (strcmp(token, "wtime") == 0)
            limits.time_ms[WHITE] = (int) v;
        else if (strcmp(token, "btime") == 0)
            limits.time_ms[BLACK] = (int) v;
        else if (strcmp(token, "winc") == 0)
            limits.inc_ms[WHITE] = (int) v;
        else if (strcmp(token, "binc") == 0)
            limits.inc_ms[BLACK] = (int) v;
        else if (strcmp(token, "movestogo") == 0)
            limits.moves_to_go = (int) v;
        else if (strcmp(token, "nodes") == 0)
            limits.nodes = (uint64_t) v;
        else if (strcmp(token, "perft") == 0) {
            report_net();
            const uint64_t n = perft(&Pos, (int) v, true);
            // Two newlines: upstream writes "\n" and then sync_endl, which adds its
            // own (uci.cpp:481). The blank line is part of the output a GUI parses.
            uci_printf("\nNodes searched: %llu\n\n", (unsigned long long) n);
            return;
        }
    }

    // Default to a bounded depth rather than an unbounded search: an unqualified
    // `go` from a script would otherwise never return.
    if (!limits.depth && !limits.movetime_ms && !limits.nodes && !limits.infinite
        && !limits.time_ms[WHITE] && !limits.time_ms[BLACK])
        limits.depth = 8;

    report_net();

    // The search zone emits `bestmove` itself, through the sink installed here, so
    // that the line is built once and in one place — upstream emits it from
    // search.cpp too. Do not print a second one from the result.
    (void) search_go(&Pos, &limits);
}

static void cmd_setoption(char *args) {
    char name[OPTION_NAME_MAX] = { 0 };

    // Hand the whole body to the table: it owns the `name ... value ...` grammar,
    // including names and values that contain spaces, and it is the same parse
    // upstream runs (ucioption.cpp:44).
    if (options_setoption(&Options, args, name) == OPTION_SET_UNKNOWN)
        uci_printf("No such option: %s\n", name);
}

static void cmd_uci(void) {
    char rendered[OPTIONS_RENDER_MAX];

    uci_printf("id name %s %s\n", ENGINE_NAME, ENGINE_VERSION);
    uci_printf("id author %s\n", ENGINE_AUTHORS);

    // The block opens with a newline of its own and closes without one, so the
    // blank line between `id author` and the first option — which upstream emits
    // and a byte-for-byte handshake diff sees — comes out of this composition and
    // not out of an extra print. Golden: uci.cpp:120.
    options_render(&Options, rendered, sizeof rendered);
    uci_write(rendered);
    uci_write("\nuciok\n");
}

// Execute one command line. Return false on `quit`.
static bool execute(char *line) {
    char *cmd = line;
    while (*cmd == ' ' || *cmd == '\t')
        ++cmd;

    // Snapshot the line before strtok chops it: a critical-error diagnostic quotes
    // the command as the operator typed it, and by the time one is raised the
    // tokeniser has already written NULs through this buffer.
    snprintf(CurrentCmd, sizeof CurrentCmd, "%s", cmd);
    for (char *e = CurrentCmd; *e; ++e)
        if (*e == '\n' || *e == '\r') {
            *e = '\0';
            break;
        }

    char *args = cmd;
    while (*args && *args != ' ' && *args != '\t' && *args != '\n')
        ++args;
    if (*args) {
        *args = '\0';
        ++args;
    }

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "stop") == 0) {
        search_stop();
        return strcmp(cmd, "quit") != 0;
    }
    if (strcmp(cmd, "uci") == 0)
        cmd_uci();
    else if (strcmp(cmd, "isready") == 0) {
        uci_printf("readyok\n");
    } else if (strcmp(cmd, "ucinewgame") == 0) {
        tt_clear();
        search_clear();
        set_position(START_FEN);
    } else if (strcmp(cmd, "position") == 0)
        cmd_position(args);
    else if (strcmp(cmd, "go") == 0)
        cmd_go(args);
    else if (strcmp(cmd, "setoption") == 0)
        cmd_setoption(args);
    else if (strcmp(cmd, "d") == 0) {
        char buf[1024];
        pos_pretty(&Pos, buf, sizeof buf);
        uci_write(buf);
    } else if (strcmp(cmd, "eval") == 0) {
        report_net();
        char buf[2048];
        evaluate_trace(&Pos, buf, sizeof buf);
        uci_write(buf);
    } else if (strcmp(cmd, "bench") == 0) {
        // Default to 13, upstream's `bench` depth (benchmark.cpp:400). The whole
        // point of the anchor is comparability with upstream's published number,
        // and any other depth searches a different tree.
        const int depth = (args && *args) ? (int) strtol(args, nullptr, 10) : BENCH_DEFAULT_DEPTH;
        benchmark_run(depth > 0 ? depth : BENCH_DEFAULT_DEPTH);
    } else if (strcmp(cmd, "compiler") == 0) {
        // Report the actual compiler: the CI builds this tree with both clang and
        // gcc, and a bug report that names the wrong one costs a round trip.
#if defined(__clang__)
        uci_printf("Compiled by clang %d.%d.%d, C%ld\n", __clang_major__, __clang_minor__,
                   __clang_patchlevel__, __STDC_VERSION__);
#elif defined(__GNUC__)
        uci_printf("Compiled by gcc %d.%d.%d, C%ld\n", __GNUC__, __GNUC_MINOR__,
                   __GNUC_PATCHLEVEL__, __STDC_VERSION__);
#else
        uci_printf("Compiled by an unknown compiler, C%ld\n", __STDC_VERSION__);
#endif
    } else if (*cmd && strcmp(cmd, "ponderhit") != 0) {
        uci_printf("Unknown command: '%s'. Type help for more information.\n", cmd);
    }

    return true;
}

// Record the directory ARGV0 was launched from, with its trailing separator, as
// the third candidate network_load searches. Leave it empty when argv[0] carries
// no separator: the working directory is already candidate two, so an empty root
// costs nothing.
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

void uci_execute(const char *line) {
    // `execute` tokenises in place with strtok, so hand it a mutable copy.
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", line);
    (void) execute(buf);
}

void uci_current_fen(char *buf, size_t buf_len) {
    char fen[128];
    pos_fen(&Pos, fen);
    snprintf(buf, buf_len, "%s", fen);
}

void uci_loop(int argc, char **argv) {
    // Announce the engine before reading a command, as upstream does from main
    // (Stockfish/src/main.cpp:40). Not decoration: it is the first line a GUI and a
    // human both use to tell which binary they launched, and its absence is exactly
    // how one build gets mistaken for another mid-measurement.
    uci_printf("%s %s by %s\n", ENGINE_NAME, ENGINE_VERSION, ENGINE_AUTHORS);

    search_set_output(emit_stdout);
    register_options();

    // Point the search at this table rather than at its own defaults, so MultiPV,
    // Skill Level, UCI_Elo, Move Overhead, nodestime, Ponder and UCI_ShowWDL are
    // read from the same place the handshake advertises them. Without this the
    // search keeps answering itself and a `setoption` a GUI sent has no effect
    // anywhere — the failure mode that leaves an option looking wired.
    search_set_option_source(option_int_for_search);

    // Size the table from the registered default rather than a second literal, so
    // `Hash`'s default and the table's initial size cannot drift apart.
    tt_resize((size_t) options_get_int(&Options, "Hash"));
    set_position(START_FEN);

    // Load the net before the first command: `go`, `go perft` and `eval` all
    // report the outcome, and a failed load leaves the classical fallback rather
    // than terminating the process.
    set_root_directory(argc > 0 ? argv[0] : nullptr);
    load_net();

    // Join the argv words into one command so `ccfish go depth 5` behaves as if
    // that line were typed, then exit without entering the loop.
    if (argc > 1) {
        char line[4096] = { 0 };
        int n = 0;
        for (int i = 1; i < argc && n < (int) sizeof line - 2; ++i)
            n += snprintf(line + n, sizeof line - (size_t) n, "%s%s", i > 1 ? " " : "", argv[i]);
        execute(line);
        tt_free();
        return;
    }

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        // Tee the command into the debug log before running it. Upstream logs on
        // the read itself (misc.cpp, Tie::uflow), so the log interleaves commands
        // and replies in the order they happened; logging after execute would put
        // every command after its own answer.
        log_bytes(line, strlen(line), ">> ");
        if (!execute(line))
            break;
    }

    tt_free();
}
