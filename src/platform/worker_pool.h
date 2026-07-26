// Host the engine's Lazy-SMP worker set on real OS threads.
//
// This is the runtime side of `engine/search/worker_set.h`: the thread pool, the NUMA
// context and the binding decision, one SearchWorker per thread and the shared history
// bank per occupied node. It lives in `platform/` because that is what it is -- OS
// threads and affinity masks -- and the zone rule allows platform to depend on engine,
// which is how it can build a SearchWorker at all.
//
// The engine never names anything in here. It declares the set it needs in
// `WorkerSetOps` and reaches it through function pointers; `worker_pool_install` is the
// one symbol the composition root calls, and everything below it is static.
//
// The model is upstream's. Every worker runs the SAME iterative deepening on the SAME
// root at slightly staggered depths, sharing the transposition table and its node's
// history bank and nothing else. There is no work splitting and no explicit
// communication.
//
// Two properties this module exists to hold:
//
//   `Threads 1` must be bit-identical to no pool at all. One worker means the pool's
//   summed counters ARE thread 0's own counters, the vote has one candidate, and no
//   binding, distribution or cross-thread synchronisation happens. That is the property
//   `./build.sh signature` rests on.
//
//   Nothing may make thread 0's behaviour depend on an address, a clock or scheduling.
//   A worker reading its own node count is deterministic; one reading the POOL's sum is
//   not, because the sum depends on when the siblings were scheduled. Only the five
//   sites upstream names read the sum -- see `engine/search/pool_source.h`.
//
// Thread 0's OS thread is spawned and left parked: `search_go` blocks, so the caller
// drives thread 0's worker inline and the pool starts only the siblings. Upstream hands
// thread 0 its own thread because its `go` returns immediately; the tree is the same
// either way.
//
// Upstream: thread.cpp (ThreadPool::set, ::clear, ::start_searching).

#ifndef MCFISH_WORKER_POOL_H
#define MCFISH_WORKER_POOL_H

// Install this pool as the engine's worker set. Call once from the composition root,
// before the first search; until then the engine runs on the one-worker set built into
// `engine/search/worker_set.c`, which searches the same tree but cannot spawn a
// second thread.
void worker_pool_install(void);

// Render the NUMA topology the pool binds under, as upstream's
// NumaConfig::to_string. malloc'd; the caller frees. This is what `Available
// processors` reports -- the installed config, not the process affinity mask.
char *worker_pool_numa_config_string(void);

#endif  // MCFISH_WORKER_POOL_H
