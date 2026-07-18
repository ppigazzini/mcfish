// Own the engine's stdio. This is the only module in the tree that writes to a
// stream, and every line the process emits passes through uci_output_print_line.
//
// The invariant is the funnel: nothing under src/engine/ may printf. The search
// emits through the sink installed by search_set_output, and
// `uci_output_emit_line` is the function with that signature — wiring it in is
// what keeps the engine zone linkable without the shell, which `build.sh
// zone-check` asserts. A line arrives WITHOUT its newline; this module appends
// exactly one and flushes, so a GUI reading line-buffered never waits.
//
// The module also holds the two pieces of cross-cutting state the search driver
// publishes and the shell reads back — the last whole-search node count, and
// quiet mode — because both sides reach this leaf and neither may reach the
// other.
//
// Port source: zfish `shell/uci_output.zig`. Golden: upstream `misc.h:73`
// (sync_cout / sync_endl), `misc.cpp:249` (the `Debug Log File` tee).

#ifndef CCFISH_UCI_OUTPUT_H
#define CCFISH_UCI_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Redirect the primary sink. Passing nullptr restores stdout. The stream is
// borrowed, never closed — that is the caller's business. This exists so a gate
// can capture output without a subprocess.
void uci_output_set_stream(FILE *stream);

// Write LEN bytes then one '\n', and flush. Ignore a write error, as upstream
// does: a closed stdout must not take the engine down mid-search.
void uci_output_print_line(const char *line, size_t len);

// Write a NUL-terminated line. This is the signature `search_set_output` and
// `engine_set_output` take, so it drops straight in as the sink.
void uci_output_emit_line(const char *line);

// Open, reopen, or close the tee `Debug Log File` names. An empty or null name
// closes any current log; a non-empty one truncates and opens for writing. A log
// that cannot be opened is reported by leaving no log open, not by failing the
// command — upstream does the same.
void uci_output_start_logger(const char *name);

// Close the log and restore the default stream. Safe without a prior open.
void uci_output_shutdown(void);

// Publish and read the node count of the last completed whole search. The bench
// driver totals these, so a `go` whose result it never sees still contributes.
void uci_output_set_last_nodes_searched(uint64_t nodes);
uint64_t uci_output_last_nodes_searched(void);
void uci_output_reset_last_nodes_searched(void);

// Suppress the search's own `info` emission for `bench` and `benchmark`, whose
// output is their own summary. Set around the run, read on the emit path.
void uci_output_set_quiet(bool quiet);
bool uci_output_is_quiet(void);

#endif  // CCFISH_UCI_OUTPUT_H
