// Run the `bench` command: compose upstream's fixed command script, drive it
// back through the dispatcher, and report the node total.
//
// The invariant is that bench runs the engine THROUGH ITS OWN UCI SURFACE. Every
// line of the script is handed to the injected dispatcher — the same one a GUI's
// input reaches — so the signature measures the shipped command path and not a
// private one that could drift from it. The dispatcher is injected rather than
// called directly so this module never imports the command loop back.
//
// Output goes to stderr, as upstream's `std::cerr` does (uci.cpp:266,
// uci.cpp:302): the banners and the summary are a report for the operator, not
// UCI traffic, and a GUI parsing stdout must not see them.
//
// Port source: zfish `shell/uci_bench.zig` (benchRuntime),
// `shell/benchmark.zig` (setupBench). Golden: upstream `uci.cpp:243`
// (UCIEngine::bench), `benchmark.cpp:126` (setup_bench).

#ifndef CCFISH_UCI_BENCH_H
#define CCFISH_UCI_BENCH_H

#include <stddef.h>
#include <stdint.h>

// Bound the composed script. Upstream's 54-entry default set expands to about
// seven kilobytes; the slack covers a `fenFile` of a few hundred positions.
enum {
    UCI_BENCH_COMMANDS_MAX = 16384,
    UCI_BENCH_LINE_MAX = 512,  // `position fen ` + one record with its moves
};

// Dispatch one command line. CTX is whatever the caller passed, untouched here.
typedef void (*UciDispatchFn)(void *ctx, const char *command);

// Compose the bench script into BUF as '\n'-separated command lines, and return
// the byte count written, excluding the NUL. Truncate rather than overrun.
//
// ARGS is `[ttSize] [threads] [limit] [fenFile] [limitType]`, defaulting to
// `16 1 13 default depth` — note that the FIRST argument is the hash size, not
// the depth. `fenFile` is `default` for the pinned set or `current` for the
// live position; a path is NOT supported here, because reading one would put
// file I/O and an exit path in a module that has neither.
//
// CURRENT_FEN supplies the `current` case and may be null otherwise.
size_t uci_bench_setup(const char *current_fen, const char *args, char *buf, size_t buf_len);

// Run the script, returning the total nodes searched. Read each `go`'s node
// count from `uci_output_last_nodes_searched`, which the search driver
// publishes — a `go` dispatched as text gives the caller no other way to see it.
// Reset the clock at each `ucinewgame`, as upstream does, so table-clearing time
// is excluded from the reported total.
uint64_t uci_bench_run(const char *args, UciDispatchFn dispatch, void *ctx);

#endif  // CCFISH_UCI_BENCH_H
