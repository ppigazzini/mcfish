// Own the UCI command dispatch: read a line, route it to an engine_* call. The
// engine session lives in engine.c; the stdio funnel lives in uci_output.c; this
// file holds neither engine state nor a stream. See uci.h.

#include "uci.h"

#include "../engine/board/board_props.h"
#include "../engine/board/position.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../platform/clock.h"
#include "../platform/tablebase.h"
#include "benchmark.h"
#include "engine.h"
#include "uci_output.h"
#include "ucioption.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_NAME "mcfish"
#define ENGINE_VERSION "dev"
#define ENGINE_AUTHORS "the Stockfish developers (see AUTHORS file)"

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Append upstream's two tablebase lines to `d`, when the position is small enough to
// be in the tables and has no castling rights (position.cpp:88-100). Probed through
// the FEN, not the live board: upstream builds a FRESH Position from pos.fen() so an
// inspection command cannot perturb the state chain the next search will walk.
static void print_tablebase_lines(void) {
    const Position *pos = engine_position();
    if (tablebase_max_cardinality() < (size_t) popcount_bb(pieces(pos)))
        return;
    if (pos->st->castling_rights != 0)
        return;

    char fen[128];
    engine_current_fen(fen, sizeof fen);
    const TbProbeResult r = tablebase_probe_fen(fen, strlen(fen), board_is_chess960(pos));

    uci_output_printf("Tablebases WDL: %4d (%d)\n", r.wdl, r.wdl_state);
    uci_output_printf("Tablebases DTZ: %4d (%d)\n", r.dtz, r.dtz_state);
}

// Report an invalid command and stop the process, as upstream does (uci.cpp:684).
// Terminating IS the contract: a GUI that sent a position the engine could not set
// must not receive a `bestmove` about some other board.
static char CurrentCmd[4096];

static void terminate_on_critical_error(const char *command, const char *reason) {
    // Two newlines: upstream writes '\n' and then sync_endl (uci.cpp:685-687).
    uci_output_printf("info string CRITICAL ERROR: Command `%s` failed. Reason: %s\n\n", command,
                      reason);
    // Tear down through the same sequence as `quit` (uci_loop return -> main.c)
    // before exiting, so the sanitized binary leaves this path leak-clean and the
    // fuzz gate stays able to flag a REAL leak. Upstream's std::exit(1) skips the
    // Engine destructor -- an automatic in main -- so freeing here diverges from
    // upstream only in memory the process was about to abandon anyway.
    engine_stop();
    search_shutdown();
    eval_nnue_shutdown();
    exit(1);
}

static void cmd_position(char *args) {
    char *token = strtok(args, " \t\n");
    if (!token)
        return;

    const char *reason = nullptr;

    if (strcmp(token, "startpos") == 0) {
        if (!engine_set_startpos(&reason))
            terminate_on_critical_error(CurrentCmd, reason);
        token = strtok(nullptr, " \t\n");
    } else if (strcmp(token, "fen") == 0) {
        // Reassemble the FEN: six space-separated fields, ending at `moves` or EOL.
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
        if (!engine_set_position(fen, &reason))
            terminate_on_critical_error(CurrentCmd, reason);
    } else {
        return;
    }

    if (token && strcmp(token, "moves") != 0)
        token = strtok(nullptr, " \t\n");

    if (token && strcmp(token, "moves") == 0)
        while ((token = strtok(nullptr, " \t\n")))
            if (!engine_play_move(token, &reason))
                terminate_on_critical_error(CurrentCmd, reason);
}

static void cmd_go(char *args) {
    // Stamp the clock as early as possible, before parsing the go arguments, so the
    // time budget is measured from when the command arrived -- not from when the search
    // thread later enters search_go (upstream uci.cpp:190, "The search starts as early
    // as possible").
    SearchLimits limits = { .start_time = (int64_t) now_ms() };

    for (char *token = strtok(args, " \t\n"); token; token = strtok(nullptr, " \t\n")) {
        // Zero-argument keywords first, reading NO lookahead token (uci.cpp:221-224).
        if (strcmp(token, "infinite") == 0) {
            limits.infinite = true;
            continue;
        }
        if (strcmp(token, "ponder") == 0) {
            limits.ponder = true;
            continue;
        }

        // Every keyword below takes exactly one argument; read it only now, with the
        // keyword already in hand (uci.cpp:192-225).
        char *value = strtok(nullptr, " \t\n");
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
            engine_report_net();
            engine_verify_network();
            const uint64_t n = engine_perft((int) v);
            // Two newlines: upstream writes "\n" then sync_endl (uci.cpp:481).
            uci_output_printf("\nNodes searched: %llu\n\n", (unsigned long long) n);
            return;
        }
    }

    // Default to a bounded depth rather than an unbounded search.
    if (!limits.depth && !limits.movetime_ms && !limits.nodes && !limits.infinite
        && !limits.time_ms[WHITE] && !limits.time_ms[BLACK])
        limits.depth = 8;

    engine_report_net();
    engine_verify_network();
    engine_go(&limits);
}

static void cmd_setoption(char *args) {
    char name[OPTION_NAME_MAX] = { 0 };
    if (engine_setoption(args, name) == OPTION_SET_UNKNOWN)
        uci_output_printf("No such option: %s\n", name);
}

static void cmd_flip(void) {
    const char *reason = nullptr;
    engine_flip(&reason);
    if (reason)
        terminate_on_critical_error(CurrentCmd, reason);
}

static void cmd_uci(void) {
    char rendered[OPTIONS_RENDER_MAX];

    uci_output_printf("id name %s %s\n", ENGINE_NAME, ENGINE_VERSION);
    uci_output_printf("id author %s\n", ENGINE_AUTHORS);

    // The block opens with a newline of its own and closes without one, so the blank
    // line between `id author` and the first option comes out of this composition.
    // Golden: uci.cpp:120.
    engine_render_options(rendered, sizeof rendered);
    uci_output_write(rendered);
    uci_output_write("\nuciok\n");
}

// Execute one command line. Return false on `quit`.
static bool execute(char *line) {
    char *cmd = line;
    while (*cmd == ' ' || *cmd == '\t')
        ++cmd;

    // Snapshot the line before strtok chops it: a critical-error diagnostic quotes
    // the command as the operator typed it.
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
        engine_stop();
        return strcmp(cmd, "quit") != 0;
    }
    if (strcmp(cmd, "uci") == 0)
        cmd_uci();
    else if (strcmp(cmd, "isready") == 0) {
        uci_output_printf("readyok\n");
    } else if (strcmp(cmd, "ucinewgame") == 0)
        engine_new_game();
    else if (strcmp(cmd, "position") == 0)
        cmd_position(args);
    else if (strcmp(cmd, "go") == 0)
        cmd_go(args);
    else if (strcmp(cmd, "setoption") == 0)
        cmd_setoption(args);
    else if (strcmp(cmd, "flip") == 0)
        cmd_flip();
    else if (strcmp(cmd, "d") == 0) {
        char buf[1024];
        engine_visualize(buf, sizeof buf);
        uci_output_write(buf);
        print_tablebase_lines();
    } else if (strcmp(cmd, "eval") == 0) {
        engine_report_net();
        engine_verify_network();
        char buf[2048];
        engine_trace_eval(buf, sizeof buf);
        uci_output_write(buf);
    } else if (strcmp(cmd, "bench") == 0) {
        // Hand the argument line to the bench, which owns upstream's whole grammar.
        // A bare `bench` is upstream's published run -- the only form the signature
        // anchor is comparable against.
        benchmark_run(args);
    } else if (strcmp(cmd, "compiler") == 0) {
#if defined(__clang__)
        uci_output_printf("Compiled by clang %d.%d.%d, C%ld\n", __clang_major__, __clang_minor__,
                          __clang_patchlevel__, __STDC_VERSION__);
#elif defined(__GNUC__)
        uci_output_printf("Compiled by gcc %d.%d.%d, C%ld\n", __GNUC__, __GNUC_MINOR__,
                          __GNUC_PATCHLEVEL__, __STDC_VERSION__);
#else
        uci_output_printf("Compiled by an unknown compiler, C%ld\n", __STDC_VERSION__);
#endif
    } else if (*cmd && strcmp(cmd, "ponderhit") != 0) {
        uci_output_printf("Unknown command: '%s'. Type help for more information.\n", cmd);
    }

    return true;
}

void uci_execute(const char *line) {
    // `execute` tokenises in place with strtok, so hand it a mutable copy.
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", line);
    (void) execute(buf);
}

void uci_current_fen(char *buf, size_t buf_len) { engine_current_fen(buf, buf_len); }

void uci_loop(int argc, char **argv) {
    // Announce the engine before reading a command, as upstream does from main
    // (main.cpp:40): the first line a GUI and a human use to tell which binary they
    // launched, and its absence is how one build gets mistaken for another.
    uci_output_printf("%s %s by %s\n", ENGINE_NAME, ENGINE_VERSION, ENGINE_AUTHORS);

    // Install the transport sinks, then build the session against them.
    engine_set_output(uci_output_emit_line, uci_output_emit_info);
    engine_init(argc > 0 ? argv[0] : nullptr);

    // Join the argv words into one command so `mcfish go depth 5` behaves as if that
    // line were typed, then exit without entering the loop.
    if (argc > 1) {
        char line[4096] = { 0 };
        int n = 0;
        for (int i = 1; i < argc && n < (int) sizeof line - 2; ++i)
            n += snprintf(line + n, sizeof line - (size_t) n, "%s%s", i > 1 ? " " : "", argv[i]);
        execute(line);
        engine_shutdown();
        return;
    }

    // Read a WHOLE line however long it is (upstream uses std::getline, unbounded --
    // uci.cpp:106): a fixed buffer split an over-long `position ... moves ...` line
    // across reads and ran each fragment as its own command.
    char *line = nullptr;
    size_t cap = 0;
    for (bool running = true; running;) {
        if (getline(&line, &cap, stdin) != -1) {
            // Tee the command into the debug log before running it, so the log
            // interleaves commands and replies in the order they happened
            // (misc.cpp, Tie::uflow).
            uci_output_log_input(line, strlen(line));
            running = execute(line);
        } else {
            // Substitute `quit` for a failed read, as upstream does
            // (uci.cpp:99-100): EOF walks the same stop-and-return path as a
            // typed quit, so a search left running by a vanished GUI is stopped
            // before teardown instead of holding the shutdown join forever.
            char eof_quit[] = "quit";
            running = execute(eof_quit);
        }
    }
    free(line);

    engine_shutdown();
}
