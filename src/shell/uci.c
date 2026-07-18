#include "uci.h"

#include "../engine/board/movegen.h"
#include "../engine/board/position.h"
#include "../engine/board/uci_move.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../engine/search/tt.h"
#include "benchmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_NAME "ccfish"
#define ENGINE_VERSION "dev"
#define ENGINE_AUTHORS "the Stockfish developers (see AUTHORS)"

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

enum { MAX_GAME_PLIES = 1024 };

// Keep the whole state chain alive for the game: pos_undo_move and the repetition
// scan both follow StateInfo::previous, so a state popped off the C stack would
// leave the chain pointing at freed memory.
static Position Pos;
static StateInfo States[MAX_GAME_PLIES];
static int StatesUsed = 0;

static struct {
    size_t hash_mb;
    int threads;
    bool chess960;
    bool ponder;
} Options = { .hash_mb = 16, .threads = 1, .chess960 = false, .ponder = false };

// Hold the EvalFile option and the directory the binary was launched from. The
// net is a runtime input, never embedded, and network_load searches "<internal>",
// the working directory, then this root — which must carry its trailing
// separator, because the concatenation inserts none.
static char EvalFile[512];
static char RootDirectory[512];

// Load the net named by the EvalFile option. Silent: a caller reports the outcome
// through report_net, so a load and a re-load read the same on the wire.
static void load_net(void) { (void) eval_nnue_load(RootDirectory, EvalFile); }

// Report the resident net, or why the classical fallback is in use. Upstream
// prints this before every go, perft and eval (engine.cpp:150, :157, :329) and
// terminates when no net loaded; ccfish keeps playing on the fallback instead, so
// the same three sites print and none exits.
static void report_net(void) {
    printf("info string %s\n", eval_nnue_status());
    fflush(stdout);
}

static void emit_stdout(const char *line) {
    printf("%s\n", line);
    fflush(stdout);
}

// Reset the position and the state chain together. Any path that sets a new
// position must come through here, or States accumulates across games.
static void set_position(const char *fen) {
    StatesUsed = 0;
    if (!pos_set(&Pos, fen, Options.chess960, &States[StatesUsed++]))
        pos_set(&Pos, START_FEN, Options.chess960, &States[0]);
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
            if (m == MOVE_NONE || StatesUsed >= MAX_GAME_PLIES)
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
            printf("\nNodes searched: %llu\n", (unsigned long long) n);
            fflush(stdout);
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
    // Parse `setoption name <NAME> value <VALUE>`; the name may contain spaces.
    char *name = strstr(args, "name ");
    if (!name)
        return;
    name += 5;

    char *value = strstr(name, " value ");
    if (value) {
        *value = '\0';
        value += 7;
        while (*value == ' ')
            ++value;
    }

    // Trim the trailing newline the reader left on the last field.
    for (char *p = name; *p; ++p)
        if (*p == '\n' || *p == '\r')
            *p = '\0';
    if (value)
        for (char *p = value; *p; ++p)
            if (*p == '\n' || *p == '\r')
                *p = '\0';

    if (strcmp(name, "Hash") == 0 && value) {
        Options.hash_mb = (size_t) strtoul(value, nullptr, 10);
        if (!tt_resize(Options.hash_mb))
            fprintf(stderr, "info string failed to allocate %zu MB hash\n", Options.hash_mb);
    } else if (strcmp(name, "Threads") == 0 && value) {
        // Accept and report the option so a GUI's handshake succeeds, but say
        // plainly that the search is single-threaded rather than silently ignoring it.
        Options.threads = (int) strtol(value, nullptr, 10);
        if (Options.threads != 1)
            printf("info string Threads is accepted but the search is single-threaded\n");
    } else if (strcmp(name, "UCI_Chess960") == 0 && value) {
        Options.chess960 = strcmp(value, "true") == 0;
    } else if (strcmp(name, "Ponder") == 0 && value) {
        Options.ponder = strcmp(value, "true") == 0;
    } else if (strcmp(name, "EvalFile") == 0 && value) {
        snprintf(EvalFile, sizeof EvalFile, "%s", value);
        load_net();
        report_net();
    } else if (strcmp(name, "Clear") == 0 || strcmp(name, "Clear Hash") == 0) {
        tt_clear();
    }
}

static void cmd_uci(void) {
    printf("id name %s %s\n", ENGINE_NAME, ENGINE_VERSION);
    printf("id author %s\n", ENGINE_AUTHORS);
    printf("option name Hash type spin default 16 min 1 max 33554432\n");
    printf("option name Threads type spin default 1 min 1 max 1\n");
    printf("option name Clear Hash type button\n");
    printf("option name Ponder type check default false\n");
    printf("option name UCI_Chess960 type check default false\n");
    printf("option name EvalFile type string default %s\n", eval_nnue_default_file_name());
    printf("uciok\n");
    fflush(stdout);
}

// Execute one command line. Return false on `quit`.
static bool execute(char *line) {
    char *cmd = line;
    while (*cmd == ' ' || *cmd == '\t')
        ++cmd;

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
        printf("readyok\n");
        fflush(stdout);
    } else if (strcmp(cmd, "ucinewgame") == 0) {
        tt_clear();
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
        printf("%s", buf);
        fflush(stdout);
    } else if (strcmp(cmd, "eval") == 0) {
        report_net();
        char buf[2048];
        evaluate_trace(&Pos, buf, sizeof buf);
        printf("%s", buf);
        fflush(stdout);
    } else if (strcmp(cmd, "bench") == 0) {
        const int depth = (args && *args) ? (int) strtol(args, nullptr, 10) : 8;
        benchmark_run(depth > 0 ? depth : 8);
    } else if (strcmp(cmd, "compiler") == 0) {
        // Report the actual compiler: the CI builds this tree with both clang and
        // gcc, and a bug report that names the wrong one costs a round trip.
#if defined(__clang__)
        printf("Compiled by clang %d.%d.%d, C%ld\n", __clang_major__, __clang_minor__,
               __clang_patchlevel__, __STDC_VERSION__);
#elif defined(__GNUC__)
        printf("Compiled by gcc %d.%d.%d, C%ld\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
               __STDC_VERSION__);
#else
        printf("Compiled by an unknown compiler, C%ld\n", __STDC_VERSION__);
#endif
        fflush(stdout);
    } else if (*cmd && strcmp(cmd, "ponderhit") != 0) {
        printf("Unknown command: '%s'. Type help for more information.\n", cmd);
        fflush(stdout);
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

void uci_loop(int argc, char **argv) {
    search_set_output(emit_stdout);
    tt_resize(Options.hash_mb);
    set_position(START_FEN);

    // Load the net before the first command: `go`, `go perft` and `eval` all
    // report the outcome, and a failed load leaves the classical fallback rather
    // than terminating the process.
    snprintf(EvalFile, sizeof EvalFile, "%s", eval_nnue_default_file_name());
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
    while (fgets(line, sizeof line, stdin))
        if (!execute(line))
            break;

    tt_free();
}
