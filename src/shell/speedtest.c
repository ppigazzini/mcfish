#include "speedtest.h"

#include "../engine/search/search.h"
#include "../platform/clock.h"
#include "../platform/memory.h"
#include "../platform/thread.h"
#include "../platform/worker_pool.h"
#include "engine.h"
#include "speedtest_positions.h"
#include "uci.h"
#include "uci_output.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Drive upstream's `speedtest`, command for command.
//
// The script is derived, not fixed: five games are replayed at a movetime that falls
// with the ply, and the whole schedule is then SCALED so the run lasts the requested
// number of seconds. That fit is upstream's and is reproduced exactly, float widths
// included -- it decides how much of the budget each position gets, so a different
// rounding is a different workload and the resulting nps is no longer comparable with
// upstream's on the same machine.
//
// Everything goes to STDERR, as upstream's does. The search itself is silenced for the
// duration (upstream nulls its five search callbacks), because 258 searches' worth of
// `info` lines is not what the operator asked for -- but the OPTION messages stay, so
// `info string Using N threads` still appears on stdout exactly as upstream leaves it.
//
// Golden: `Stockfish/src/uci.cpp:315` (UCIEngine::benchmark) and
// `Stockfish/src/benchmark.cpp:466` (setup_benchmark).

enum {
    // Probably not very important for a test this long, but included for completeness
    // and sanity (uci.cpp:317).
    NUM_WARMUP_POSITIONS = 3,

    // TT_SIZE_PER_THREAD is chosen such that roughly half of the hash is used once all
    // positions for the current sequence have been searched (benchmark.cpp:468).
    TT_SIZE_PER_THREAD = 128,

    DEFAULT_DURATION_S = 150,
};

// Fit the per-move time from LTC games: seconds = 50/(ply+15), so the 10th move gets
// 2000ms before scaling (benchmark.cpp:479-486).
static double corrected_time(int ply) { return 50000.0 / ((double) ply + 15.0); }

// Read one integer field, or leave *OUT at its default and report that the line ran
// dry. Upstream extracts with `is >> field`, so the FIRST failure leaves that field
// and every field after it at its default -- and the invocation echo prints only the
// fields that were actually given.
static bool next_int(const char **cursor, int *out) {
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    if (*p == '\0') {
        *cursor = p;
        return false;
    }

    char *end = nullptr;
    const long v = strtol(p, &end, 10);
    if (end == p || v < INT_MIN || v > INT_MAX) {
        // A token that is not a number puts the stream in a fail state upstream, and
        // every later extraction then fails too. Park the cursor on the terminator so
        // the remaining fields take their defaults.
        *cursor = p + strlen(p);
        return false;
    }
    *cursor = end;
    *out = (int) v;
    return true;
}

// Count the go commands the run will issue: one per position of every game.
static int count_positions(void) {
    int n = 0;
    for (int g = 0; g < SpeedtestGameCount; ++g)
        for (const char *const *fen = SpeedtestGames[g]; *fen != nullptr; ++fen)
            ++n;
    return n;
}

// Run one position: set it, search it for MOVETIME ms, and return what it searched.
// NODES and ELAPSED_MS accumulate the SEARCH alone -- upstream stamps its clock in the
// on-start callback and stops it on bestmove, so neither the position parse nor the
// per-game table clear is in the number this command divides by.
static void run_position(const char *fen, int movetime, uint64_t *nodes, uint64_t *elapsed_ms) {
    char line[256];
    snprintf(line, sizeof line, "position fen %s", fen);
    uci_execute(line);

    snprintf(line, sizeof line, "go movetime %d", movetime);
    search_reset_last_nodes_searched();

    const uint64_t start = now_ms();
    // Bypass the command loop's `go` announcement, as upstream's own benchmark does:
    // it parses limits itself and calls Engine::go directly.
    uci_bench_go(line);
    search_wait();
    *elapsed_ms += now_ms() - start;

    *nodes += search_last_nodes_searched();
}

void speedtest_run(const char *args) {
    int threads = (int) thread_hardware_concurrency();
    int tt_size = 0;
    int desired_time_s = DEFAULT_DURATION_S;

    // The echo is two strings, and the difference between them is the point: the USER
    // invocation shows what was typed, the FILLED one shows what ran. A bug report
    // pasting the block can then be reproduced without knowing this host's core count.
    char original[64] = { 0 };
    const char *cursor = args != nullptr ? args : "";
    size_t original_len = 0;

    if (next_int(&cursor, &threads))
        original_len +=
          (size_t) snprintf(original + original_len, sizeof original - original_len, "%d", threads);
    tt_size = TT_SIZE_PER_THREAD * threads;
    if (next_int(&cursor, &tt_size))
        original_len += (size_t) snprintf(original + original_len, sizeof original - original_len,
                                          " %d", tt_size);
    if (next_int(&cursor, &desired_time_s))
        (void) snprintf(original + original_len, sizeof original - original_len, " %d",
                        desired_time_s);

    char filled[64];
    snprintf(filled, sizeof filled, "%d %d %d", threads, tt_size, desired_time_s);

    // Scale the whole schedule to the requested duration. The accumulation is float
    // and the fit is double, exactly as upstream mixes them: the product is truncated
    // to an int millisecond count, so a wider accumulator moves individual movetimes.
    float total_time = 0;
    for (int g = 0; g < SpeedtestGameCount; ++g) {
        int ply = 1;
        for (const char *const *fen = SpeedtestGames[g]; *fen != nullptr; ++fen)
            total_time += (float) corrected_time(ply++);
    }
    const float scale = (float) (desired_time_s * 1000) / total_time;

    const int total_positions = count_positions();

    // Silence the LINE sink for the duration -- the search's `info`/`bestmove` lines
    // and the per-`go` net banner -- and leave the INFO sink installed, which is the
    // split upstream makes: it nulls its five search callbacks and keeps the option
    // listener, so `info string Using N threads` below still prints.
    engine_set_output(nullptr, uci_output_emit_info);

    char setopt[64];
    snprintf(setopt, sizeof setopt, "setoption name Threads value %d", threads);
    uci_execute(setopt);
    snprintf(setopt, sizeof setopt, "setoption name Hash value %d", tt_size);
    uci_execute(setopt);
    uci_execute("setoption name UCI_Chess960 value false");

    // Warmup: the first three positions, at the movetimes they will get in the real
    // run, so the caches and the table are warm and the first measured position is not
    // paying for the process's first search.
    int warmed = 0;
    uint64_t discard_nodes = 0, discard_ms = 0;
    for (int g = 0; g < SpeedtestGameCount && warmed < NUM_WARMUP_POSITIONS; ++g) {
        uci_execute("ucinewgame");
        int ply = 1;
        for (const char *const *fen = SpeedtestGames[g];
             *fen != nullptr && warmed < NUM_WARMUP_POSITIONS; ++fen) {
            // One newline is produced by the search, so omit it here (uci.cpp:344).
            fprintf(stderr, "\rWarmup position %d/%d", ++warmed, NUM_WARMUP_POSITIONS);
            run_position(*fen, (int) (corrected_time(ply++) * (double) scale), &discard_nodes,
                         &discard_ms);
        }
    }
    fprintf(stderr, "\n");

    uci_execute("ucinewgame");  // may take a while: it clears the whole table

    // Sample the table's fill at two ages after every search: 0 is what this search
    // touched, 999 is everything the game has. Upstream hardcodes exactly these two
    // for the display and says so in a static_assert.
    int32_t max_hashfull[2] = { 0, 0 };
    int64_t total_hashfull[2] = { 0, 0 };
    int hashfull_readings = 0;
    static constexpr int32_t HashfullAges[2] = { 0, 999 };

    uint64_t nodes = 0, elapsed_ms = 0;
    int position = 0;

    for (int g = 0; g < SpeedtestGameCount; ++g) {
        uci_execute("ucinewgame");
        int ply = 1;
        for (const char *const *fen = SpeedtestGames[g]; *fen != nullptr; ++fen) {
            fprintf(stderr, "\rPosition %d/%d", ++position, total_positions);
            run_position(*fen, (int) (corrected_time(ply++) * (double) scale), &nodes, &elapsed_ms);

            ++hashfull_readings;
            for (int i = 0; i < 2; ++i) {
                const int32_t hf = search_hashfull(HashfullAges[i]);
                max_hashfull[i] = hf > max_hashfull[i] ? hf : max_hashfull[i];
                total_hashfull[i] += hf;
            }
        }
    }

    engine_set_output(uci_output_emit_line, uci_output_emit_info);

    // Ensure positivity to avoid a 'divide by zero' (uci.cpp:439).
    const uint64_t total_ms = elapsed_ms < 1 ? 1 : elapsed_ms;
    const int readings = hashfull_readings < 1 ? 1 : hashfull_readings;

    char version[64], compiler[COMPILER_INFO_MAX];
    uci_engine_version(version, sizeof version);
    uci_compiler_info(compiler, sizeof compiler);

    char *const numa = worker_pool_numa_config_string();
    char *const binding = worker_pool_thread_binding_string();

    fprintf(stderr,
            "\n"
            "===========================\n"
            "Version                    : %s"
            "%s"  // opens with its own newline and closes with one
            "Large pages                : %s\n"
            "User invocation            : speedtest %s\n"
            "Filled invocation          : speedtest %s\n"
            "Available processors       : %s\n"
            "Thread count               : %d\n"
            "Thread binding             : %s\n"
            "TT size [MiB]              : %d\n"
            "Hash max, avg [per mille]  : \n"
            "    single search          : %d, %d\n"
            "    single game            : %d, %d\n"
            "Total nodes searched       : %" PRIu64 "\n"
            "Total search time [s]      : %g\n"
            "Nodes/second               : %" PRIu64 "\n",
            version, compiler, has_large_pages() ? "yes" : "no", original, filled,
            numa != nullptr ? numa : "", threads,
            binding != nullptr && binding[0] != '\0' ? binding : "none", tt_size,
            (int) max_hashfull[0], (int) (total_hashfull[0] / readings), (int) max_hashfull[1],
            (int) (total_hashfull[1] / readings), nodes, (double) total_ms / 1000.0,
            nodes * 1000 / total_ms);

    free(numa);
    free(binding);
}
