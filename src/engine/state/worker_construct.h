// Construct and tear down a SearchWorker: allocate the block, bind its references, write
// its identity, and run the clear that fills its tables.
//
// The order is the contract. The block is ZEROED before anything is written -- a reused
// block would otherwise carry the previous worker's root position and PV buffers into
// fields the depth loop writes only once it has a root, which is exactly the kind of
// address dependence the signature gate exists to catch. Then the references bind, then
// the clear runs, and only then is the worker a legal argument to the search.
//
// The refresh cache is seeded from the network's feature-transformer biases, so a worker
// built before a net is resident is complete except for that cache;
// `worker_ensure_network` finishes the job after the load.
//
// Upstream: search.cpp (Worker::Worker, Worker::clear), thread.cpp:48 (Thread::Thread).

#ifndef MCFISH_WORKER_CONSTRUCT_H
#define MCFISH_WORKER_CONSTRUCT_H

#include "worker_layout.h"

#include <stdbool.h>
#include <stddef.h>

// Collect what the constructor receives: the references the engine shares, plus this
// worker's identity within the pool and within its NUMA node.
typedef struct {
    SharedHistories *shared_history;  // this worker's NUMA node's bank
    ThreadPool *threads;
    size_t thread_idx;
    size_t numa_thread_idx;
    size_t numa_total;
    size_t numa_access_token;
} WorkerCtorInputs;

// Allocate and build one worker, or return null. Thread 0 gets a SearchManager, the
// siblings get none -- upstream's NullSearchManager with the virtual call removed.
//
// Call this ON the thread that will run the worker, so the block is first-touched on the
// NUMA node that will read it.
SearchWorker *worker_create(const WorkerCtorInputs *in);

// Release the worker and everything it owns. Idempotent on null.
void worker_destroy(SearchWorker *w);

// Run the clear: this worker's history tables, its stripe of the node's shared tables,
// and its refresh cache. Reach this on `ucinewgame`, never per `go`.
void worker_clear(SearchWorker *w);

// Re-seed the refresh cache from the resident net's feature-transformer biases when the
// net has changed since this worker last did, and record the generation. Return true
// when the cache now matches the resident net.
//
// Reach this at the top of every search, not only on `ucinewgame`: a worker is built
// before the shell loads the net, and an EvalFile change reloads it under a worker that
// is already running. Upstream: search.h:331 (Worker::ensure_network_replicated).
bool worker_ensure_network(SearchWorker *w);

#endif  // MCFISH_WORKER_CONSTRUCT_H
