// Hold the `bench` command's fixed script: upstream's Defaults, verbatim.
//
// This is pure data and nothing else — no search, no stdio, no dependency. The
// invariant is that the table is IDENTICAL to upstream's, entry for entry and in
// order: `bench` is the repo's anchor, so the position set is what the node
// signature is a fact about. Changing an entry is a behaviour change, and one
// that cannot be compared against upstream afterwards.
//
// A `setoption ...` entry is dispatched as-is; every other entry is a position
// record, some with a `moves ...` suffix, to be prefixed with `position fen `.
// uci_bench.c composes the script from them.
//
// This table IS what pins tools/signature.golden: `benchmark_run` composes its
// script from `BenchDefaults` and nothing else. Adding, removing or reordering a
// line moves the anchor and invalidates every comparison against upstream, so
// re-derive it with `./build.sh signature-update` on a green gate and say in the
// commit what moved it.
//
// Golden: upstream `benchmark.cpp:31` (Defaults).

#ifndef MCFISH_BENCH_POSITIONS_H
#define MCFISH_BENCH_POSITIONS_H

// Read the bench script. Static storage, immutable, valid for the process.
extern const char *const BenchDefaults[];
extern const int BenchDefaultsCount;

#endif  // MCFISH_BENCH_POSITIONS_H
