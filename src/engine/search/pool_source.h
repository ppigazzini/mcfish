// Report the pool's summed counters to the search zone without naming the pool.
//
// Upstream reads `threads.nodes_searched()` and `threads.tb_hits()` at exactly five
// places -- `check_time`, `Worker::elapsed`, `output_pv`, the `nodes as time` settle and
// the best-move-change collection -- and the worker's OWN counter everywhere else. The
// distinction is not cosmetic: a value drawn from the pool sum depends on when the other
// threads were scheduled, so a search decision taken on one is not reproducible. Route
// only the five, and only through here.
//
// Every hook is null until the driver installs it, and a null hook reads as "this
// worker's own value". AT ONE THREAD THE TWO ARE THE SAME NUMBER -- the sum is over one
// worker, and that worker is thread 0 -- which is what keeps the anchor bit-exact across
// the wiring.
//
// Upstream: search.cpp:2075, :2090 (check_time), :1864 (elapsed), :2235, :2239
// (output_pv), :236 (advance_nodes_time), :563 (bestMoveChanges).

#ifndef MCFISH_POOL_SOURCE_H
#define MCFISH_POOL_SOURCE_H

#include <stdint.h>

typedef struct {
    void *ctx;
    uint64_t (*nodes)(void *ctx);
    uint64_t (*tb_hits)(void *ctx);
    // Sum every worker's best-move-change counter AND reset each to zero, which is one
    // operation upstream (search.cpp:563-564) and must stay one here: a sum that does
    // not reset double-counts the next iteration's instability.
    uint64_t (*collect_best_move_changes)(void *ctx);
} PoolSource;

extern PoolSource PoolCounters;

#endif  // MCFISH_POOL_SOURCE_H
