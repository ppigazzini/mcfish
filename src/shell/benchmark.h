// Run the fixed benchmark that produces the node signature.
//
// The signature is the repo's anchor: upstream's default position list, searched
// to a fixed depth through the engine's own UCI surface, yields one node total,
// and any change to move generation, ordering, pruning, or evaluation moves it.
// `build.sh signature` asserts it against tools/signature.golden. Read the
// expected value from that file, never from memory or from a doc.
//
// Golden: `Stockfish/src/benchmark.cpp: setup_bench` and
// `Stockfish/src/uci.cpp: UCIEngine::bench`.

#ifndef CCFISH_BENCHMARK_H
#define CCFISH_BENCHMARK_H

#include <stdint.h>

// Search to upstream's depth unless the caller names another
// (Stockfish/src/benchmark.cpp:400).
enum { BENCH_DEFAULT_DEPTH = 13 };

// Run the bench script at DEPTH and return the total node count. Print the
// per-position banners and the summary to stderr, matching upstream's stream choice.
uint64_t benchmark_run(int depth);

#endif  // CCFISH_BENCHMARK_H
