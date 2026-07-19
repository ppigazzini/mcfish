// Construct and tear down a Worker: bind its references, write its identity scalars, and
// run the clear that fills its tables.
//
// The order is the contract. The block must be ZEROED before anything is written -- one
// field is set by neither the constructor nor the clear, so a reused block would otherwise
// leave it heap-layout-dependent, which is exactly the kind of address dependence the
// signature gate exists to catch. Then the references bind, then the clear runs, and only
// then is the Worker a legal argument to the search.
//
// The refresh cache is seeded from the network's feature-transformer biases, so a Worker
// built before a net is resident is complete except for that cache. Construction reports
// it rather than refusing, and worker_seed_refresh_cache finishes the job after the load.
//
// Upstream: search.cpp (Worker::Worker, Worker::clear), search.cpp:696 (reductions fill).
// Port source: zfish src/engine/state/worker_construct.zig.

#ifndef MCFISH_WORKER_CONSTRUCT_H
#define MCFISH_WORKER_CONSTRUCT_H

#include "shared_history_types.h"
#include "shared_state.h"
#include "worker_layout.h"

#include <stdbool.h>
#include <stddef.h>

// Collect what the constructor receives: the references unpacked from the SharedState,
// plus this thread's identity within the pool and its NUMA node.
typedef struct {
    SharedHistories *shared_history;  // this worker's NUMA node's bank
    ThreadPool *threads;
    TranspositionTable *tt;
    SearchManager *manager;  // thread 0's manager; null on the siblings
    size_t thread_idx;
    size_t numa_thread_idx;
    size_t numa_total;
    size_t numa_access_token;
} WorkerCtorInputs;

// Fill IN from SS and the thread identity, so a caller holding a SharedState does not
// unpack it by hand.
void worker_ctor_inputs_from_shared(WorkerCtorInputs *in,
                                    const SharedState *ss,
                                    SearchManager *manager,
                                    size_t thread_idx,
                                    size_t numa_thread_idx,
                                    size_t numa_total,
                                    size_t numa_access_token);

// Write only the members the constructor owns: the references, the identity scalars, and
// the accumulator stack's initial single live slot. W must already be zeroed and have had
// its arena pointers bound by worker_block_init.
void worker_write_constructor_fields(Worker *w, const WorkerCtorInputs *in);

// Run the clear: the history tables, this worker's stripe of the shared tables, the
// reduction table, and the accumulator stack reset. Leaves the refresh cache alone.
void worker_clear(Worker *w);

// Fill REDUCTIONS[1 ..] with upstream's log schedule. Entry 0 stays 0.
void worker_fill_reductions(int *reductions, size_t count);

// Seed the refresh cache from the resident net's feature-transformer biases. Return false
// when no net is loaded, leaving the cache untouched.
bool worker_seed_refresh_cache(Worker *w);

// Build a complete Worker into BLOCK -- a worker_block_alloc block, or any zeroed,
// WORKER_ALIGN-aligned block of worker_block_bytes bytes. Allocate the root move list,
// bind the arenas, write the constructor fields, clear, and seed the refresh cache.
//
// Return null when the root move list cannot be allocated. Set *NETWORK_READY, when it is
// non-null, to whether the refresh cache was seeded -- a false there is not a failure, it
// means worker_seed_refresh_cache must run once a net is loaded.
Worker *worker_construct_full(void *block, const WorkerCtorInputs *in, bool *network_ready);

// Release what the Worker owns -- the root move list -- without freeing the block. Pair
// with worker_block_free, which the block's allocator owns.
void worker_destruct(Worker *w);

#endif  // MCFISH_WORKER_CONSTRUCT_H
