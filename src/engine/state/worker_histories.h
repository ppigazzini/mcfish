// Own the per-Worker history block and the reference it holds to the shared tables.
//
// The split is upstream's: the butterfly, low-ply, capture, continuation and
// continuation-correction tables plus the tt-move counter belong to ONE worker and are
// written without synchronisation; the correction and pawn tables are shared per NUMA
// node and are indexed by a Zobrist key. history.h's `Histories` is the flat block that
// carries both, so a worker embeds one and the shared reference points into whichever
// block owns the shared bank -- its own when there is one worker per node.
//
// Clearing is STRIPED. Every worker on a node clears the same shared table, so each takes
// the slice `[thread_idx * n / total, (thread_idx + 1) * n / total)`. With one worker the
// slice is the whole table, which is why the single-threaded clear is identical to
// upstream's whether or not the stripe is honoured -- and why the stripe must still be
// honoured, or two workers race and a third range is never cleared at all.
//
// Upstream: search.h:332 (Worker history members), search.cpp (Worker::clear). Port
// source: zfish src/engine/state/worker_histories.zig.

#ifndef CCFISH_WORKER_HISTORIES_H
#define CCFISH_WORKER_HISTORIES_H

#include "../search/history.h"
#include "shared_history_types.h"

#include <stddef.h>

typedef struct {
    // Own the per-worker tables. The correction and pawn members of this block are the
    // node's shared bank when this worker owns it, and are otherwise unread -- reach them
    // only through `shared_history`.
    Histories tables;

    // Reference the node's shared tables. Null until worker_histories_bind_shared runs.
    SharedHistories *shared_history;
} WorkerHistories;

// Point WH's shared reference at SH. SH stays owned by whoever bound it to a block.
void worker_histories_bind_shared(WorkerHistories *wh, SharedHistories *sh);

// Bind WH's shared reference to WH's OWN block, the single-node arrangement: the worker
// owns the shared bank as well as its private tables.
void worker_histories_bind_own(WorkerHistories *wh, SharedHistories *sh);

// Reset this worker's own block to the cleared values, through history_clear. That also
// refills the block's shared sub-tables, which is what a worker owning its node's bank
// wants and is unread for one that does not; a worker referencing someone else's bank
// still reaches it only through `shared_history`.
void worker_histories_clear(WorkerHistories *wh);

// Clear this worker's stripe of the shared tables. THREAD_IDX is the worker's index
// within its NUMA node and TOTAL is that node's worker count; TOTAL 0 is treated as 1.
void worker_histories_clear_shared(SharedHistories *sh, size_t thread_idx, size_t total);

#endif  // CCFISH_WORKER_HISTORIES_H
