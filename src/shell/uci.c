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
#include "speedtest.h"
#include "uci_output.h"
#include "ucioption.h"

#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
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
        //
        // A SEPARATOR AFTER EVERY FIELD, INCLUDING THE LAST -- upstream builds this
        // string as `fen += token + " "` (uci.cpp:512), and the trailing space is
        // load-bearing because Position::set reads the result as a CHARACTER stream:
        // a FEN cut short after the side-to-move field ends in whitespace there and
        // is accepted, where the same text without it fails "Expected whitespace
        // after side to move". Joining with separators BETWEEN fields made mcfish's
        // parser see a different string than upstream's does.
        char fen[128] = { 0 };
        int n = 0;
        while ((token = strtok(nullptr, " \t\n")) && strcmp(token, "moves") != 0) {
            const int len = (int) strlen(token);
            if (n + len + 2 >= (int) sizeof fen)
                break;
            memcpy(fen + n, token, (size_t) len);
            n += len;
            fen[n++] = ' ';
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

// Read one `go` argument AT THE WIDTH OF THE FIELD IT GOES INTO, terminating the way
// upstream does when it does not fit.
//
// `is >> field` fails when the text does not fit the FIELD's type, and upstream turns
// that failbit into a critical error on the very next line (uci.cpp:229-230), so the
// declared width is observable from a GUI. Everything here was parsed as one wide
// integer, which is too wide for the five `int` fields and too narrow for `nodes`:
//
//   go depth 3000000000               upstream: CRITICAL ERROR   mcfish: searched
//   go nodes 999999999999999999999999 upstream: CRITICAL ERROR   mcfish: searched
//
// LO/HI are the field's own range: a C++ stream sets failbit when the parsed value
// falls outside the target type, so the bound IS the accept/reject boundary rather
// than something to clamp to.
static long long go_value_int(const char *key, const char *value, long long lo, long long hi) {
    char *end = nullptr;
    errno = 0;
    const long long v = value != nullptr ? strtoll(value, &end, 10) : 0;
    if (value == nullptr || end == value || errno == ERANGE || v < lo || v > hi) {
        char reason[64];
        snprintf(reason, sizeof reason, "Invalid argument for '%s'", key);
        terminate_on_critical_error(CurrentCmd, reason);
    }
    return v;
}

// Bound a clock WHERE IT ENTERS, and say so.
//
// The five clocks arrive as a signed TimePoint straight off the wire and reach
// timeman.c's arithmetic unvalidated, where a magnitude the protocol never had a right
// to send is signed overflow:
//
//   go wtime 1000 winc -9223372036854775808
//     src/engine/search/timeman.c:67:51 signed integer overflow:
//       -9223372036854775808 * 49 cannot be represented in type 'int64_t'
//   go wtime 4000000000000000000 winc 4000000000000000000 btime 1000
//     src/engine/search/timeman.c:67:51 signed integer overflow: 4000000000000000000 * 49
//
// That is not a defect in the arithmetic. It is asked to hold a number nothing bounded,
// and the parser is the only place that knows the number came from outside. The width
// check above stays what it is -- the accept/reject boundary a GUI can observe, which
// upstream's failbit defines -- so this clamps the VALUE without moving that boundary.
//
// The bound is 1e12 ms, about 31 years: past any real time control, and far enough
// below the top that every product timeman forms stays inside a TimePoint. The one
// divergence from upstream is a clock between 1e12 and the overflow threshold, where
// upstream's arithmetic is still defined -- a time control no game has and no GUI
// sends. Outside the bound is reported, never silently taken.
static constexpr long long MaxClockMs = 1000000000000;

static long long go_clock(const char *key, const char *value) {
    const long long given = go_value_int(key, value, INT64_MIN, INT64_MAX);
    const long long bounded = given < 0 ? 0 : (given > MaxClockMs ? MaxClockMs : given);
    if (bounded != given)
        uci_output_printf("info string %s %lld is outside [0, %lld]; using %lld\n", key, given,
                          MaxClockMs, bounded);
    return bounded;
}

// Bound `movestogo` and `mate` WHERE THEY ENTER, the way go_clock bounds the clocks.
//
// Both are read straight off the wire into an `int` and both reach arithmetic that has
// no room for the extremes of that type:
//
//   go wtime 60000 btime 60000 winc 1000000000000 movestogo -2147483648
//     src/engine/search/timeman.c:67:51 signed integer overflow:
//       1000000000000 * -2147483649 cannot be represented in type 'int64_t'
//   go mate 2147483647
//     src/engine/search/search_id.c:384:64 signed integer overflow:
//       2 * 2147483647 cannot be represented in type 'int'
//
// timeman already widens `movestogo` to int64_t, so the plain `mtg - 1` upstream
// overflows on is safe here and only the product with a clock at the 1e12 bound
// reaches it; `2 * mate` is still an `int` because upstream's stop condition is.
//
// The bounds are each field's own domain. `movestogo` counts moves, so a negative is
// not a shorter time control but a value the protocol never had a right to send, and 0
// is what timeman already reads as "absent". `mate` is halved because the condition
// doubles it. As with go_clock this clamps the VALUE without moving the accept/reject
// boundary the width check above defines, and outside the bound is reported, never
// silently taken.
static long long go_bounded_int(const char *key, const char *value, long long lo, long long hi) {
    const long long given = go_value_int(key, value, INT_MIN, INT_MAX);
    const long long bounded = given < lo ? lo : (given > hi ? hi : given);
    if (bounded != given)
        uci_output_printf("info string %s %lld is outside [%lld, %lld]; using %lld\n", key, given,
                          lo, hi, bounded);
    return bounded;
}

// `nodes` is upstream's `u64`, read the way a C++ stream reads one: a leading minus is
// ACCEPTED and the magnitude wraps, so `go nodes -1` is a budget of UINT64_MAX on both
// sides, while a magnitude past UINT64_MAX is a critical error.
static uint64_t go_value_u64(const char *key, const char *value) {
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = value != nullptr ? strtoull(value, &end, 10) : 0;
    if (value == nullptr || end == value || errno == ERANGE) {
        char reason[64];
        snprintf(reason, sizeof reason, "Invalid argument for '%s'", key);
        terminate_on_critical_error(CurrentCmd, reason);
    }
    return v;
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

    // PERFT IS A LIMIT LIKE ANY OTHER while the line is being read, and only a
    // dispatch once the whole line has been read: upstream parses first and then
    // tests `if (limits.perft)` (uci.cpp:437-441). So a ZERO is not a perft at all --
    // it is an ordinary search with no limit -- and running the divide from inside
    // the loop, on the keyword's mere presence, made `go perft 0` print a depth-zero
    // divide of every root move where upstream plays a move.
    int perft_depth = 0;

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
        long long slot_lo = INT_MIN;
        long long slot_hi = INT_MAX;
        int64_t *clock_slot = nullptr;
        bool wants_nodes = false;
        bool wants_perft = false;
        if (strcmp(token, "depth") == 0)
            slot = &limits.depth;
        else if (strcmp(token, "movetime") == 0)
            clock_slot = &limits.movetime_ms;
        else if (strcmp(token, "wtime") == 0)
            clock_slot = &limits.time_ms[WHITE];
        else if (strcmp(token, "btime") == 0)
            clock_slot = &limits.time_ms[BLACK];
        else if (strcmp(token, "winc") == 0)
            clock_slot = &limits.inc_ms[WHITE];
        else if (strcmp(token, "binc") == 0)
            clock_slot = &limits.inc_ms[BLACK];
        else if (strcmp(token, "movestogo") == 0) {
            slot = &limits.moves_to_go;
            slot_lo = 0;
        } else if (strcmp(token, "mate") == 0) {
            slot = &limits.mate;
            slot_lo = 0;
            slot_hi = INT_MAX / 2;
        } else if (strcmp(token, "nodes") == 0)
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

        if (slot != nullptr)
            *slot = (int) go_bounded_int(token, value, slot_lo, slot_hi);
        else if (clock_slot != nullptr)
            *clock_slot = go_clock(token, value);
        else if (wants_nodes)
            limits.nodes = go_value_u64(token, value);
        else if (wants_perft)
            perft_depth = (int) go_value_int(token, value, INT_MIN, INT_MAX);
    }

    // The dispatch, after the WHOLE line is parsed and on the VALUE, not the keyword.
    if (perft_depth != 0) {
        engine_report_net();
        engine_verify_network();
        const uint64_t n = engine_perft(perft_depth);
        // Two newlines: upstream writes "\n" then sync_endl (uci.cpp:481).
        uci_output_printf("\nNodes searched: %llu\n\n", (unsigned long long) n);
        return;
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
// Append to BUF at *POS, never past LEN. One helper so the block below reads as the
// sequence of fields it is rather than as bounds arithmetic.
[[gnu::format(printf, 4, 5)]] static void
append(char *buf, size_t len, size_t *pos, const char *fmt, ...) {
    if (*pos >= len)
        return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf + *pos, len - *pos, fmt, args);
    va_end(args);
    if (n > 0)
        *pos += (size_t) n < len - *pos ? (size_t) n : len - *pos - 1;
}

void uci_compiler_info(char *buf, size_t buf_len) {
    size_t pos = 0;
    buf[0] = '\0';

    append(buf, buf_len, &pos, "\nCompiled by                : ");
#if defined(__clang__)
    append(buf, buf_len, &pos, "clang++ %d.%d.%d", __clang_major__, __clang_minor__,
           __clang_patchlevel__);
#elif defined(__GNUC__)
    append(buf, buf_len, &pos, "g++ (GNUC) %d.%d.%d", __GNUC__, __GNUC_MINOR__,
           __GNUC_PATCHLEVEL__);
#else
    append(buf, buf_len, &pos, "Unknown compiler (unknown version)");
#endif
#if defined(__linux__)
    append(buf, buf_len, &pos, " on Linux");
#elif defined(__APPLE__)
    append(buf, buf_len, &pos, " on Apple");
#elif defined(_WIN64)
    append(buf, buf_len, &pos, " on Microsoft Windows 64-bit");
#else
    append(buf, buf_len, &pos, " on unknown system");
#endif

    append(buf, buf_len, &pos, "\nCompilation architecture   : ");
#if defined(MCFISH_ARCH_STRING)
    append(buf, buf_len, &pos, "%s", MCFISH_ARCH_STRING);
#else
    append(buf, buf_len, &pos, "(undefined architecture)");
#endif

    append(buf, buf_len, &pos, "\nCompilation settings       : %s",
           sizeof(void *) == 8 ? "64bit" : "32bit");
#if defined(__AVX512VBMI2__) && defined(__AVX512BITALG__)
    append(buf, buf_len, &pos, " AVX512ICL");
#endif
#if defined(__AVX512VNNI__)
    append(buf, buf_len, &pos, " VNNI");
#endif
#if defined(__AVX512F__)
    append(buf, buf_len, &pos, " AVX512");
#endif
#if defined(__BMI2__)
    append(buf, buf_len, &pos, " BMI2");
#endif
#if defined(__AVX2__)
    append(buf, buf_len, &pos, " AVX2");
#endif
#if defined(__SSE4_1__)
    append(buf, buf_len, &pos, " SSE41");
#endif
#if defined(__SSSE3__)
    append(buf, buf_len, &pos, " SSSE3");
#endif
#if defined(__SSE2__)
    append(buf, buf_len, &pos, " SSE2");
#endif
#if defined(__POPCNT__)
    append(buf, buf_len, &pos, " POPCNT");
#endif
#if !defined(NDEBUG)
    append(buf, buf_len, &pos, " DEBUG");
#endif

    append(buf, buf_len, &pos, "\nCompiler __VERSION__ macro : ");
#ifdef __VERSION__
    append(buf, buf_len, &pos, "%s", __VERSION__);
#else
    append(buf, buf_len, &pos, "(undefined macro)");
#endif
    // ONE trailing newline, not two. Upstream's `compiler_info()` ends there and its
    // two callers add what they need: the `compiler` command's `sync_endl` makes the
    // blank line below, and `speedtest` writes its next field straight on.
    append(buf, buf_len, &pos, "\n");
}

void uci_engine_version(char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%s %s", ENGINE_NAME, ENGINE_VERSION);
}

static void cmd_compiler(void) {
    char info[COMPILER_INFO_MAX];
    uci_compiler_info(info, sizeof info);
    uci_output_write(info);
    uci_output_write("\n");
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

// Print upstream's help blurb, byte for byte (uci.cpp:175-183).
//
// It names STOCKFISH rather than this port, and deliberately: the text is a citation
// of the project this is a clone of, it points at that project's README, and the
// goldens are adjudicated against the oracle -- a paraphrase would be a divergence the
// gate reports on every run. The identity line `uci_loop` prints is the one place the
// port names itself. Both files the last sentence promises are shipped here too.
//
// The leading and trailing blank lines are upstream's: the literal opens with a
// newline and closes with one, and `sync_endl` adds a second.
static void cmd_help(void) {
    uci_output_write(
      "\nStockfish is a powerful chess engine for playing and analyzing."
      "\nIt is released as free software licensed under the GNU GPLv3 License."
      "\nStockfish is normally used with a graphical user interface (GUI) and implements"
      "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
      "\nFor any further information, visit https://github.com/official-stockfish/Stockfish#readme"
      "\nor read the corresponding README.md and Copying.txt files distributed along with this "
      "program.\n\n");
}

// Execute one command line. Return false on `quit`.
static bool execute(char *line) {
    char *cmd = line;
    while (*cmd == ' ' || *cmd == '\t')
        ++cmd;

    // Snapshot the line before strtok chops it: a critical-error diagnostic and the
    // unknown-command reply both quote the command as the operator typed it. Snapshot
    // `line` rather than the token, indent included -- upstream assigns `currentCmd`
    // the whole getline result before it extracts a token (uci.cpp:106-107), so the
    // leading whitespace it prints back is the whitespace that was typed.
    snprintf(CurrentCmd, sizeof CurrentCmd, "%s", line);
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

    // A blank line and a `#` line are not commands: upstream reaches neither the
    // dispatch nor its unknown-command reply for them, because every arm hangs off
    // `!token.empty() && token[0] != '#'` (uci.cpp:181, added by 5346f1c6c so a
    // commented script can be piped at the engine). Answer them where upstream does
    // -- by doing nothing at all. Returning HERE rather than from the else-chain
    // below is what makes that true: the chain is entered past `engine_end_search()`,
    // so a comment between two `go` lines would otherwise drain a running search
    // while printing nothing, and a script's comments would change its search.
    if (*cmd == '\0' || *cmd == '#')
        return true;

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
    } else if (strcmp(cmd, "speedtest") == 0) {
        // Upstream spells the command through a constant (`BenchmarkCommand`) and
        // reports it back in the invocation echo; the name is the only place it
        // appears, so it is spelled here and in speedtest.c's two echo lines.
        speedtest_run(args);
    } else if (strcmp(cmd, "export_net") == 0) {
        // One token, as `is >> filename` reads (uci.cpp:165-172): a path with a space
        // in it is cut at the space on both engines, and no argument at all means the
        // default name -- which the net policy then refuses unless the resident net IS
        // the default one.
        const char *file = strtok(args, " \t\n");
        engine_export_net(file);
    } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0
               || strcmp(cmd, "--license") == 0 || strcmp(cmd, "license") == 0) {
        cmd_help();
    } else {
        // Quote the WHOLE line, not the token: upstream prints the `cmd` it read
        // (uci.cpp:182), so `positon startpos` comes back in full and the operator can
        // see the typo in the reply. The empty and `#` lines the same upstream arm
        // excludes have already returned above.
        uci_output_printf("Unknown command: '%s'. Type help for more information.\n", CurrentCmd);
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
    // line were typed, then exit without entering the loop. Each word carries a
    // TRAILING space, as upstream's `cmd += argv[i] + " "` does (uci.cpp:97): every
    // parser reads the same tokens either way, but the unknown-command reply quotes
    // the line it was handed, so `mcfish frobnicate` must come back with the space
    // upstream shows.
    if (argc > 1) {
        char line[4096] = { 0 };
        int n = 0;
        for (int i = 1; i < argc && n < (int) sizeof line - 2; ++i)
            n += snprintf(line + n, sizeof line - (size_t) n, "%s ", argv[i]);
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
