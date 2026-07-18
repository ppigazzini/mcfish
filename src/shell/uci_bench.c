#include "uci_bench.h"

#include "../engine/board/position.h"
#include "../platform/clock.h"
#include "bench_positions.h"
#include "engine.h"
#include "uci_output.h"
#include "uci_parse.h"
#include "uci_strings.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Hold the composed script for the duration of one run. `bench` is a single
// command on a single-threaded listener, so one buffer is enough and keeps the
// protocol path free of allocation.
static char CommandsBuf[UCI_BENCH_COMMANDS_MAX];

// ---------------------------------------------------------------------------
// Script composition
// ---------------------------------------------------------------------------

static void append_command(UciBuf *b, const char *command) {
    if (b->len != 0)
        uci_buf_append_char(b, '\n');
    uci_buf_append_str(b, command);
}

static void append_command_n(UciBuf *b, const char *command, size_t len) {
    if (b->len != 0)
        uci_buf_append_char(b, '\n');
    uci_buf_append(b, command, len);
}

// Search a counted span for a NUL-terminated needle. Written out rather than
// calling memmem, which is a GNU extension the build's _POSIX_C_SOURCE does not
// expose.
static bool contains(const char *haystack, size_t len, const char *needle) {
    const size_t needle_len = strlen(needle);
    if (needle_len > len)
        return false;
    for (size_t i = 0; i + needle_len <= len; ++i)
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return true;
    return false;
}

// Emit the two lines one position contributes: the `position fen` that sets it,
// and the `go`/`eval` that measures it. A `setoption` entry is a script
// directive and passes through untouched.
static void append_position_line(UciBuf *b, const char *line, size_t len, const char *go) {
    // Match on `setoption` anywhere in the line, as upstream's `find` does.
    if (contains(line, len, "setoption")) {
        append_command_n(b, line, len);
        return;
    }

    if (b->len != 0)
        uci_buf_append_char(b, '\n');
    uci_buf_append_str(b, "position fen ");
    uci_buf_append(b, line, len);
    append_command(b, go);
}

// Copy one token out as a NUL-terminated string, or fall back to DEFAULT_VALUE
// when the token stream is exhausted.
static void take_arg(UciTokens *it, const char *default_value, char *out, size_t out_len) {
    const char *token;
    size_t len;
    if (!uci_tokens_next(it, &token, &len)) {
        snprintf(out, out_len, "%s", default_value);
        return;
    }
    if (len > out_len - 1)
        len = out_len - 1;
    memcpy(out, token, len);
    out[len] = '\0';
}

size_t uci_bench_setup(const char *current_fen, const char *args, char *buf, size_t buf_len) {
    char tt_size[32];
    char threads[32];
    char limit[32];
    char fen_file[64];
    char limit_type[32];

    UciTokens it;
    uci_tokens_init(&it, args ? args : "", args ? strlen(args) : 0);
    take_arg(&it, "16", tt_size, sizeof tt_size);
    take_arg(&it, "1", threads, sizeof threads);
    take_arg(&it, "13", limit, sizeof limit);
    take_arg(&it, "default", fen_file, sizeof fen_file);
    take_arg(&it, "depth", limit_type, sizeof limit_type);

    // `eval` names no limit, so it is the command itself rather than a `go`.
    char go[64];
    if (strcmp(limit_type, "eval") == 0)
        snprintf(go, sizeof go, "eval");
    else
        snprintf(go, sizeof go, "go %s %s", limit_type, limit);

    UciBuf b;
    uci_buf_init(&b, buf, buf_len);

    // Set the table size and thread count first, then clear: a bench whose hash
    // carried over from the session would not be the same measurement twice.
    uci_buf_appendf(&b, "setoption name Threads value %s", threads);
    uci_buf_appendf(&b, "\nsetoption name Hash value %s", tt_size);
    append_command(&b, "ucinewgame");

    if (strcmp(fen_file, "current") == 0) {
        if (current_fen && current_fen[0] != '\0')
            append_position_line(&b, current_fen, strlen(current_fen), go);
    } else {
        // Anything that is not `current` runs the pinned set. Upstream also
        // accepts a path here and reads the positions from it; that path is
        // unported — see PORT_NOTES_uci.md.
        for (int i = 0; i < BenchDefaultsCount; ++i)
            append_position_line(&b, BenchDefaults[i], strlen(BenchDefaults[i]), go);
    }

    return b.len;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

static bool first_token(const char *line, size_t len, const char **token, size_t *token_len) {
    UciTokens it;
    uci_tokens_init(&it, line, len);
    return uci_tokens_next(&it, token, token_len);
}

static uint64_t count_bench_positions(const char *commands, size_t len) {
    uint64_t total = 0;

    UciLines lines;
    uci_lines_init(&lines, commands, len);

    const char *line;
    size_t line_len;
    while (uci_lines_next(&lines, &line, &line_len)) {
        const char *token;
        size_t token_len;
        if (!first_token(line, line_len, &token, &token_len))
            continue;
        if (uci_token_equals(token, token_len, "go") || uci_token_equals(token, token_len, "eval"))
            ++total;
    }
    return total;
}

static void current_fen(char *buf, size_t buf_len) {
    Position *pos = engine_get_position();
    if (!pos) {
        if (buf_len != 0)
            buf[0] = '\0';
        return;
    }
    pos_fen(pos, buf);
}

uint64_t uci_bench_run(const char *args, UciDispatchFn dispatch, void *ctx) {
    char fen[UCI_FEN_MAX];
    current_fen(fen, sizeof fen);

    const size_t commands_len = uci_bench_setup(fen, args, CommandsBuf, sizeof CommandsBuf);

    const uint64_t total_positions = count_bench_positions(CommandsBuf, commands_len);
    uint64_t nodes = 0;
    uint64_t position_index = 1;
    int64_t elapsed_start = (int64_t) now_ms();

    // Hold the command as a NUL-terminated string: the dispatcher takes one, and
    // the script's lines are spans into CommandsBuf. The longest line the script
    // can hold is `position fen ` plus one record with its moves.
    char command[UCI_BENCH_LINE_MAX];

    UciLines lines;
    uci_lines_init(&lines, CommandsBuf, commands_len);

    const char *line;
    size_t line_len;
    while (uci_lines_next(&lines, &line, &line_len)) {
        const char *token;
        size_t token_len;
        if (!first_token(line, line_len, &token, &token_len))
            continue;

        if (line_len > sizeof command - 1)
            line_len = sizeof command - 1;
        memcpy(command, line, line_len);
        command[line_len] = '\0';

        const bool is_go = uci_token_equals(token, token_len, "go");
        const bool is_eval = uci_token_equals(token, token_len, "eval");

        if (is_go || is_eval) {
            current_fen(fen, sizeof fen);
            fprintf(stderr, "\nPosition: %" PRIu64 "/%" PRIu64 " (%s)\n", position_index,
                    total_positions, fen);
            ++position_index;

            if (is_go) {
                ParsedLimits limits;
                uci_parse_limits(command, strlen(command), &limits);

                if (limits.perft != 0) {
                    nodes += engine_perft(limits.perft);
                } else {
                    // The search publishes its node count through the output
                    // module; reset first so a `go` that emits nothing cannot
                    // count the previous position twice.
                    uci_output_reset_last_nodes_searched();
                    dispatch(ctx, command);
                    nodes += uci_output_last_nodes_searched();
                }
            } else {
                dispatch(ctx, command);
            }
            continue;
        }

        dispatch(ctx, command);
        if (uci_token_equals(token, token_len, "ucinewgame"))
            elapsed_start = (int64_t) now_ms();
    }

    // Add one so the division below can never divide by zero on a run fast
    // enough to finish inside the clock's resolution.
    int64_t elapsed = (int64_t) now_ms() - elapsed_start + 1;
    if (elapsed <= 0)
        elapsed = 1;

    const uint64_t elapsed_u64 = (uint64_t) elapsed;
    const uint64_t nps = nodes * 1000 / elapsed_u64;

    fprintf(stderr,
            "\n===========================\n"
            "Total time (ms) : %" PRId64 "\n"
            "Nodes searched  : %" PRIu64 "\n"
            "Nodes/second    : %" PRIu64 "\n",
            elapsed, nodes, nps);

    return nodes;
}
