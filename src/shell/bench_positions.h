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
// NOTE: `src/shell/benchmark.c` still carries `BenchFens`, a REDUCED 16-position
// set that pins tools/signature.golden today. The two are deliberately separate:
// this one is upstream's, that one is ccfish's current anchor. Cutting the
// signature over to this table is an intended behaviour change and moves the
// golden — see docs/PORTING.md on re-deriving it.
//
// Port source: zfish `shell/bench_positions.zig`. Golden: upstream
// `benchmark.cpp:31` (Defaults).

#ifndef CCFISH_BENCH_POSITIONS_H
#define CCFISH_BENCH_POSITIONS_H

// Read the bench script. Static storage, immutable, valid for the process.
extern const char *const BenchDefaults[];
extern const int BenchDefaultsCount;

#endif  // CCFISH_BENCH_POSITIONS_H
