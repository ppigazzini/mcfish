// Inject the UCI output sink for the search's info / bestmove lines.
//
// Writing to the UCI stream is a shell service, so the search hands its formatted
// lines to function pointers the shell registers at startup rather than calling
// printf. The defaults drop the line and report not-quiet, so a headless build
// still runs the whole formatting path (and is therefore still fuzz-exercised)
// while producing no output.
//
// Treat these three as DEGRADED, not safe, when unregistered: the search computes
// the same move, but `bestmove` is dropped with everything else. That is a wrong
// answer that happens not to be a wrong MOVE, and it is defensible only because
// the shell registers all three before the engine is reachable.
//
// Ported from zfish `engine/search/output_sink.zig`.

#ifndef MCFISH_OUTPUT_SINK_H
#define MCFISH_OUTPUT_SINK_H

#include <stddef.h>
#include <stdint.h>

// Write one already-formatted UCI line (no trailing newline).
// failure: silent — drops every line including `bestmove`.
extern void (*OutputPrintLine)(const char *str, size_t len);

// Report whether output is suppressed (bench / quiet mode).
// failure: silent — not-quiet, so the line building still runs.
extern bool (*OutputIsQuiet)(void);

// Publish the whole-search node count, read back by the bench signature line.
// failure: silent — discards it; no shell means no signature line to read it.
extern void (*OutputSetLastNodesSearched)(uint64_t nodes);

#endif  // MCFISH_OUTPUT_SINK_H
