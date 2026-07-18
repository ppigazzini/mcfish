// Run the fixed benchmark that produces the node signature.
//
// The signature is the repo's anchor: a fixed position set searched to a fixed
// depth with a cleared TT yields one node total, and any change to move
// generation, ordering, pruning, or evaluation moves it. `build.sh signature`
// asserts it against tools/signature.golden. Read the expected value from that
// file, never from memory or from a doc.

#ifndef CCFISH_BENCHMARK_H
#define CCFISH_BENCHMARK_H

#include <stdint.h>

// Search the bench set at DEPTH and return the total node count. Print the
// per-position banners and the summary to stderr, matching upstream's stream choice.
uint64_t benchmark_run(int depth);

// Expose the bench set for the perft gate, which walks the same positions.
extern const char *const BenchFens[];
extern const int BenchFenCount;

#endif  // CCFISH_BENCHMARK_H
