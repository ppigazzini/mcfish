#include "benchmark.h"

#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../platform/clock.h"
#include "bench_positions.h"
#include "uci.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Drive upstream's `bench`, command for command.
//
// The script is fixed and its shape IS the measurement:
//
//   setoption name Threads value 1
//   setoption name Hash value 16
//   ucinewgame                       <- once, and only here
//   position fen <record>            <- for each entry of BenchDefaults
//   go depth <depth>
//
// The single `ucinewgame` is load-bearing. Upstream clears the transposition
// table, the history block and the per-game manager scalars exactly once and then
// lets all three CARRY from one position to the next; clearing per position is a
// different search and a different number. The rest follows from that: the
// entries run in order, and a `setoption` entry is dispatched as-is, because the
// Chess960 positions near the end are only legal under the option it sets.
//
// Golden: `Stockfish/src/benchmark.cpp:430` (setup_bench) and
// `Stockfish/src/uci.cpp:243` (UCIEngine::bench).

// Bound the composed line at `position fen ` plus the widest record in the table,
// which carries a FEN and a `moves ...` suffix.
enum { BENCH_LINE_MAX = 256 };

// Bound a FEN file. Upstream reads an unbounded vector; this refuses past the cap
// rather than growing, and the cap is far above any suite anyone benches with.
enum { BENCH_FILE_MAX = 512 };

// Report whether an entry is a script directive rather than a position. Upstream
// matches `setoption` anywhere in the string (benchmark.cpp:435), not at the head.
static bool is_setoption(const char *entry) { return strstr(entry, "setoption") != nullptr; }

static int count_positions(void) {
    int n = 0;
    for (int i = 0; i < BenchDefaultsCount; ++i)
        if (!is_setoption(BenchDefaults[i]))
            ++n;
    return n;
}

static int count_file_positions(const char fens[][128], int count) {
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (!is_setoption(fens[i]))
            ++n;
    return n;
}

// Read the next whitespace-separated field, or leave DST holding its default.
// Upstream fills each field from the stream and keeps its default when the stream
// runs dry, so a short line means "defaults from here on" rather than an error
// (benchmark.cpp:394-398).
static const char *next_field(const char **cursor, char *dst, size_t cap, const char *fallback) {
    snprintf(dst, cap, "%s", fallback);
    if (!cursor || !*cursor)
        return dst;
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (!*p) {
        *cursor = p;
        return dst;
    }
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        ++p;
    const size_t len = (size_t) (p - start) < cap - 1 ? (size_t) (p - start) : cap - 1;
    memcpy(dst, start, len);
    dst[len] = '\0';
    *cursor = p;
    return dst;
}

uint64_t benchmark_run(const char *args) {
    // Name the evaluation the run used, on the banner stream the totals go to. The
    // signature anchor is defined with a net loaded, and a fallback run produces an
    // unrelated number, so the two must never be told apart by eye alone.
    fprintf(stderr, "info string %s\n", eval_nnue_status());

    char tt_size[32], threads[32], limit[32], fen_file[256], limit_type[32];
    const char *cursor = args;
    next_field(&cursor, tt_size, sizeof tt_size, "16");
    next_field(&cursor, threads, sizeof threads, "1");
    next_field(&cursor, limit, sizeof limit, "13");
    next_field(&cursor, fen_file, sizeof fen_file, "default");
    next_field(&cursor, limit_type, sizeof limit_type, "depth");

    // `eval` takes no limit; every other type spells one (benchmark.cpp:401).
    char go[64];
    if (strcmp(limit_type, "eval") == 0)
        snprintf(go, sizeof go, "eval");
    else
        snprintf(go, sizeof go, "go %s %s", limit_type, limit);

    // Read the position list. `current` is whatever is set now, so it must be read
    // BEFORE the loop starts moving the board.
    char current_fen[128] = { 0 };
    const bool use_current = strcmp(fen_file, "current") == 0;
    const bool use_default = strcmp(fen_file, "default") == 0;
    if (use_current)
        uci_current_fen(current_fen, sizeof current_fen);

    static char file_fens[BENCH_FILE_MAX][128];
    int file_count = 0;
    if (!use_current && !use_default) {
        FILE *f = fopen(fen_file, "r");
        if (!f) {
            fprintf(stderr, "Unable to open file %s\n", fen_file);
            exit(EXIT_FAILURE);
        }
        while (file_count < BENCH_FILE_MAX && fgets(file_fens[file_count], 128, f)) {
            char *nl = strpbrk(file_fens[file_count], "\r\n");
            if (nl)
                *nl = '\0';
            if (file_fens[file_count][0])
                ++file_count;
        }
        fclose(f);
    }

    const int total_positions = use_current ? 1
                              : use_default ? count_positions()
                                            : count_file_positions(file_fens, file_count);
    int position_index = 1;
    uint64_t nodes = 0;

    // Size and thread the table before the clear, so `ucinewgame` starts the game
    // on the table the whole run then shares.
    char setopt[64];
    snprintf(setopt, sizeof setopt, "setoption name Threads value %s", threads);
    uci_execute(setopt);
    snprintf(setopt, sizeof setopt, "setoption name Hash value %s", tt_size);
    uci_execute(setopt);
    uci_execute("ucinewgame");

    const uint64_t start = now_ms();

    const int entries = use_current ? 1 : (use_default ? BenchDefaultsCount : file_count);
    for (int i = 0; i < entries; ++i) {
        const char *entry =
          use_current ? current_fen : (use_default ? BenchDefaults[i] : file_fens[i]);

        // Dispatch a `setoption` entry as-is wherever the list came from: upstream
        // applies this filter to a FEN file exactly as to the default list
        // (benchmark.cpp:435), so a file may carry the Chess960 toggles too.
        if (!use_current && is_setoption(entry)) {
            uci_execute(entry);
            continue;
        }

        char line[BENCH_LINE_MAX];
        snprintf(line, sizeof line, "position fen %s", entry);
        uci_execute(line);

        char fen[128];
        uci_current_fen(fen, sizeof fen);
        fprintf(stderr, "\nPosition: %d/%d (%s)\n", position_index++, total_positions, fen);

        // Take the count the search published rather than a return value: that is
        // the number upstream's `on_update_full` capture sums. Reset first, or a
        // root with no legal moves publishes nothing and counts its predecessor
        // twice (Stockfish/src/uci.cpp:270).
        search_reset_last_nodes_searched();
        uci_execute(go);
        nodes += search_last_nodes_searched();
    }

    // Add one so the division can never divide by zero on a run fast enough to
    // finish inside the clock's resolution (Stockfish/src/uci.cpp:299).
    const uint64_t elapsed = now_ms() - start + 1;

    fprintf(stderr,
            "\n===========================\n"
            "Total time (ms) : %" PRIu64 "\n"
            "Nodes searched  : %" PRIu64 "\n"
            "Nodes/second    : %" PRIu64 "\n",
            elapsed, nodes, nodes * 1000 / elapsed);

    return nodes;
}
