// Own the UCI command dispatch: read a line, route it to an engine_* call. The
// engine session lives in engine.c; the stdio funnel lives in uci_output.c; this
// file holds neither engine state nor a stream. See uci.h.

#include "uci.h"

#include "../engine/board/board_props.h"
#include "../engine/board/position.h"
#include "../engine/board/uci_move.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../platform/clock.h"
#include "../platform/tablebase.h"
#include "benchmark.h"
#include "engine.h"
#include "uci_output.h"
#include "ucioption.h"

#include <ctype.h>
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
        // Consume the `moves` token, if any -- and upstream consumes ONE token here
        // whatever it turns out to be (uci.cpp:508), which is why `position startpos
        // fen <FEN>` is an error rather than a position: `fen` is eaten as the
        // keyword and the FEN's own words are then read as MOVES.
        (void) strtok(nullptr, " \t\n");
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

    // Whatever is left on the line is a MOVE, with no keyword to introduce it: both
    // branches above have already consumed the `moves` token if there was one
    // (uci.cpp:516-521). Skipping a second token here instead let a stray word slip
    // between `startpos` and `moves`, and swallowed the shape upstream rejects.
    while ((token = strtok(nullptr, " \t\n")))
        if (!engine_play_move(token, &reason))
            terminate_on_critical_error(CurrentCmd, reason);
}

// ANNOUNCE is what separates the two callers, and it is upstream's own split rather
// than a convenience. The command loop prints the processor and thread info strings on
// the `go` line (uci.cpp:133-138); UCIEngine::bench does NOT go through that loop --
// it parses the limits itself and calls perft/engine.go directly (uci.cpp:266-289), so
// a bench position emits neither. Routing bench through the announcing path put two
// extra info strings under every `Position: n/m` banner.
static void go_line(char *args, bool announce) {
    // execute() has already drained any prior search before dispatching here, so the
    // clock stamped now measures from this `go`, and the prior search's output has
    // already flushed ahead of this command's net banner.

    // Upstream sends these two on the `go` LINE, before it looks at a single argument
    // and before it decides between a search and a perft (uci.cpp:133-138), for old
    // GUIs and python-chess that do not read info strings before the first search. So
    // `go perft` gets them too. Emitting them after the argument parse instead put them
    // on the search path only, because the perft arm returns from inside the loop --
    // which is what tools/upstream_golden_audit.sh found on its first run, against two
    // goldens that were green because they were photographs of mcfish.
    if (announce)
        engine_report_threads();

    // Stamp the clock after them and before parsing the go arguments, which is where
    // upstream stamps it: `now()` is the first line of its parse_limits (uci.cpp:190,
    // "The search starts as early as possible"), so the two info strings are already
    // out. The budget still measures from when the command arrived rather than from
    // when the search thread later enters search_go.
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

        // `searchmoves` CONSUMES THE REST OF THE LINE and must therefore be last
        // (uci.cpp:196 says so in as many words). Convert here, against the position
        // the `go` is about: upstream stores the strings and converts in
        // start_thinking against the same board, and drops whatever `to_move` cannot
        // resolve -- an unknown, malformed or ILLEGAL move all come back none. That
        // is also why upstream needs no separate legality filter downstream.
        //
        // Lower-cased first, as upstream does: `to_lower(token)` there, so "E2E4"
        // resolves rather than silently dropping out of the list.
        if (strcmp(token, "searchmoves") == 0) {
            while ((token = strtok(nullptr, " \t\n")) != nullptr) {
                char lowered[16];
                size_t n = 0;
                for (; token[n] != '\0' && n < sizeof lowered - 1; ++n)
                    lowered[n] = (char) tolower((unsigned char) token[n]);
                lowered[n] = '\0';

                const Move m = move_from_uci(engine_position(), lowered);
                if (m != MOVE_NONE && limits.searchmoves_count < MAX_MOVES)
                    limits.searchmoves[limits.searchmoves_count++] = m;
            }
            break;
        }

        // RECOGNISE THE KEYWORD BEFORE READING A VALUE, which is the order upstream's
        // parse_limits works in and the reason it can reject a bad argument at all: it
        // extracts only for a keyword it knows, then tests `is.fail()` (uci.cpp:192-231).
        // Reading the lookahead first — as this did — swallows the next token for an
        // unknown keyword, and leaves nothing to complain about when the value is
        // missing or is not a number.
        //
        // Every token upstream's parse_limits knows is here now. `searchmoves` is
        // handled above, because it consumes the rest of the line rather than one
        // value.
        int *slot = nullptr;
        bool wants_nodes = false;
        bool wants_perft = false;
        if (strcmp(token, "depth") == 0)
            slot = &limits.depth;
        else if (strcmp(token, "movetime") == 0)
            slot = &limits.movetime_ms;
        else if (strcmp(token, "wtime") == 0)
            slot = &limits.time_ms[WHITE];
        else if (strcmp(token, "btime") == 0)
            slot = &limits.time_ms[BLACK];
        else if (strcmp(token, "winc") == 0)
            slot = &limits.inc_ms[WHITE];
        else if (strcmp(token, "binc") == 0)
            slot = &limits.inc_ms[BLACK];
        else if (strcmp(token, "movestogo") == 0)
            slot = &limits.moves_to_go;
        else if (strcmp(token, "mate") == 0)
            slot = &limits.mate;
        else if (strcmp(token, "nodes") == 0)
            wants_nodes = true;
        else if (strcmp(token, "perft") == 0)
            wants_perft = true;
        else
            continue;  // Unknown token: upstream extracts nothing and ignores it.

        // A recognised keyword MUST be followed by a usable number. Upstream's
        // `is >> field` sets failbit when the value is absent or unparseable, and the
        // very next line terminates the process; mcfish took `strtol("abc")` == 0 and
        // searched on, so `go depth` silently became the default depth and `go wtime
        // xyz` silently became zero time.
        //
        // Trailing garbage is NOT an error, matching the stream: `is >> int` on "5x"
        // reads 5 and leaves "x" to be read as its own (unknown, ignored) token. So
        // the test is "did any digit parse at all", not "was the whole token consumed".
        const char *value = strtok(nullptr, " \t\n");
        char *end = nullptr;
        const long v = value != nullptr ? strtol(value, &end, 10) : 0;
        if (value == nullptr || end == value) {
            char reason[64];
            snprintf(reason, sizeof reason, "Invalid argument for '%s'", token);
            terminate_on_critical_error(CurrentCmd, reason);
        }

        if (slot != nullptr)
            *slot = (int) v;
        else if (wants_nodes)
            limits.nodes = (uint64_t) v;
        else if (wants_perft) {
            engine_report_net();
            engine_verify_network();
            const uint64_t n = engine_perft((int) v);
            // Two newlines: upstream writes "\n" then sync_endl (uci.cpp:481).
            uci_output_printf("\nNodes searched: %llu\n\n", (unsigned long long) n);
            return;
        }
    }

    // NO DEFAULT DEPTH. A `go` with no usable limit searches until `stop`, which is
    // what upstream does: its limit fields are plain ints and every test of them is a
    // truthiness check, so an absent limit and a ZERO limit are the same thing and
    // neither bounds the depth loop (search.cpp's `!(limits.depth && rootDepth >
    // limits.depth)`).
    //
    // mcfish capped these at depth 8. That was defensible when `go` ran on the UCI
    // thread -- an unbounded search there could not be interrupted, because the loop
    // that would read `stop` was inside it -- and it stopped being defensible when the
    // search moved onto worker 0. It made `go`, `go nodes 0`, `go depth 0` and `go
    // movetime 0` all stop at depth 8 where upstream was still going at 23.
    //
    // The zero-limit half falls out for free: search_control already tests
    // `lim_movetime != 0` and `lim_nodes != 0`, and search_id tests `limits_depth != 0`,
    // so every zero already meant "absent" everywhere below this line. Only this
    // default disagreed.

    // The net banner comes last, after the two above and after the argument parse:
    // upstream reaches it through Engine::go's own verify callback, not from the
    // command loop.
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

// Answer `compiler` in upstream's SHAPE: a leading blank line, four fields aligned
// on a 27-column label, and a trailing blank line (misc.cpp compiler_info).
//
// The CONTENT has to differ -- clang built this, not g++, and the ISA set is this
// tree's own -- but the LAYOUT is upstream's, because a bug report pastes this block
// verbatim and a reader should not have to learn a second layout to read it.
// mcfish answered with one sentence, "Compiled by clang 22.1.8, C202311", which
// carried neither the architecture nor the settings a report needs.
//
// The feature list is upstream's ORDER, not the compiler's: widest first, then BMI2,
// then the narrowing SSE tiers, POPCNT last. Each entry is gated on the macro the
// compiler defines for the flag build.sh passed, so the line describes THIS binary
// rather than what the tier is nominally supposed to have.
static void cmd_compiler(void) {
    uci_output_printf("\nCompiled by                : ");
#if defined(__clang__)
    uci_output_printf("clang++ %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    uci_output_printf("g++ (GNUC) %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    uci_output_printf("Unknown compiler (unknown version)");
#endif
#if defined(__linux__)
    uci_output_printf(" on Linux");
#elif defined(__APPLE__)
    uci_output_printf(" on Apple");
#elif defined(_WIN64)
    uci_output_printf(" on Microsoft Windows 64-bit");
#else
    uci_output_printf(" on unknown system");
#endif

    uci_output_printf("\nCompilation architecture   : ");
#if defined(MCFISH_ARCH_STRING)
    uci_output_printf("%s", MCFISH_ARCH_STRING);
#else
    uci_output_printf("(undefined architecture)");
#endif

    uci_output_printf("\nCompilation settings       : %s", sizeof(void *) == 8 ? "64bit" : "32bit");
#if defined(__AVX512VBMI2__) && defined(__AVX512BITALG__)
    uci_output_printf(" AVX512ICL");
#endif
#if defined(__AVX512VNNI__)
    uci_output_printf(" VNNI");
#endif
#if defined(__AVX512F__)
    uci_output_printf(" AVX512");
#endif
#if defined(__BMI2__)
    uci_output_printf(" BMI2");
#endif
#if defined(__AVX2__)
    uci_output_printf(" AVX2");
#endif
#if defined(__SSE4_1__)
    uci_output_printf(" SSE41");
#endif
#if defined(__SSSE3__)
    uci_output_printf(" SSSE3");
#endif
#if defined(__SSE2__)
    uci_output_printf(" SSE2");
#endif
#if defined(__POPCNT__)
    uci_output_printf(" POPCNT");
#endif
#if !defined(NDEBUG)
    uci_output_printf(" DEBUG");
#endif

    uci_output_printf("\nCompiler __VERSION__ macro : ");
#ifdef __VERSION__
    uci_output_printf("%s", __VERSION__);
#else
    uci_output_printf("(undefined macro)");
#endif
    uci_output_printf("\n\n");
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

    // The four commands a GUI sends DURING a search -- stop, quit, isready, ponderhit --
    // answer without draining it: that is the whole point of running the search off this
    // thread. Everything else waits for the search below.
    if (strcmp(cmd, "stop") == 0) {
        // Raise the stop flag and return to the loop at once. The search thread sees
        // it, ends, and emits its `bestmove` on its own thread -- the UCI thread does
        // not block waiting for that.
        engine_stop();
        return true;
    }
    if (strcmp(cmd, "quit") == 0) {
        // End a running search before leaving the loop, so teardown does not free the
        // TT or net under a search thread still reading them. A bounded search finishes
        // (its output stays deterministic); an unbounded one is stopped so quit -- and
        // the EOF that substitutes it when a GUI vanishes -- cannot hang.
        engine_end_search();
        return false;
    }
    if (strcmp(cmd, "isready") == 0) {
        // A ping: upstream answers it even mid-search (uci.cpp), so do not drain.
        uci_output_printf("readyok\n");
        return true;
    }
    if (strcmp(cmd, "ponderhit") == 0) {
        engine_ponderhit();
        return true;
    }

    // Every remaining command either mutates state the running search reads (position,
    // setoption, ucinewgame) or prints output that must land after the search's lines
    // (d, eval, uci, bench). End the search first so none of them races it or jumps
    // ahead of its output: a bounded search is waited out (its output stays complete),
    // an unbounded one is stopped so the command cannot hang behind an endless search.
    // The during-search commands above have already returned.
    engine_end_search();

    if (strcmp(cmd, "uci") == 0)
        cmd_uci();
    else if (strcmp(cmd, "ucinewgame") == 0)
        engine_new_game();
    else if (strcmp(cmd, "position") == 0)
        cmd_position(args);
    else if (strcmp(cmd, "go") == 0)
        go_line(args, true);
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
        cmd_compiler();
    } else if (*cmd) {
        uci_output_printf("Unknown command: '%s'. Type help for more information.\n", cmd);
    }

    return true;
}

void uci_bench_go(const char *line) {
    // Take the same `go ...` line the command loop would, minus the announcement.
    // benchmark.c cannot reach go_line through uci_execute without it.
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", line);
    char *args = buf;
    if (strncmp(args, "go", 2) == 0)
        args += 2;
    go_line(args, false);
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
