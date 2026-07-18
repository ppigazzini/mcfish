#include "benchmark.h"

#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../platform/clock.h"
#include "bench_positions.h"
#include "uci.h"

#include <inttypes.h>
#include <stdio.h>
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

uint64_t benchmark_run(int depth) {
    // Name the evaluation the run used, on the banner stream the totals go to. The
    // signature anchor is defined with a net loaded, and a fallback run produces an
    // unrelated number, so the two must never be told apart by eye alone.
    fprintf(stderr, "info string %s\n", eval_nnue_status());

    char go[32];
    snprintf(go, sizeof go, "go depth %d", depth);

    const int total_positions = count_positions();
    int position_index = 1;
    uint64_t nodes = 0;

    // Size and thread the table before the clear, so `ucinewgame` starts the game
    // on the table the whole run then shares.
    uci_execute("setoption name Threads value 1");
    uci_execute("setoption name Hash value 16");
    uci_execute("ucinewgame");

    const uint64_t start = now_ms();

    for (int i = 0; i < BenchDefaultsCount; ++i) {
        if (is_setoption(BenchDefaults[i])) {
            uci_execute(BenchDefaults[i]);
            continue;
        }

        char line[BENCH_LINE_MAX];
        snprintf(line, sizeof line, "position fen %s", BenchDefaults[i]);
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
