// Own the Lazy-SMP worker set: the thread pool, one SearchWorker per thread, the shared
// history bank per NUMA node, and the two answers only the whole set can give -- the
// summed counters and the thread vote.
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
//   sites upstream names read the sum -- see pool_source.h.
//
// Thread 0's OS thread is spawned and left parked: `search_go` blocks, so the caller
// drives thread 0's worker inline and the pool starts only the siblings. Upstream hands
// thread 0 its own thread because its `go` returns immediately; the tree is the same
// either way.
//
// Upstream: thread.cpp (ThreadPool::set, ::clear, ::start_searching, ::get_best_thread).

#ifndef MCFISH_SEARCH_THREADS_H
#define MCFISH_SEARCH_THREADS_H

#include "../../platform/thread_pool.h"
#include "../state/worker_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Build COUNT workers, replacing any current set. Return false on OOM or a refused
// spawn, in which case the set is left EMPTY -- a caller must re-check
// `search_threads_main`. A COUNT of 0 is read as 1: a search needs a worker.
bool search_threads_set(size_t count);

// Choose the NUMA policy the next `search_threads_set` binds under. Return false when
// POLICY names no node at all, leaving the previous topology in place -- a config with
// zero nodes makes every distribution and binding decision divide by zero.
bool search_threads_set_numa_policy(const char *policy);

size_t search_threads_count(void);

// Return thread 0's worker, building a one-thread set on first use. Null only when that
// allocation failed.
SearchWorker *search_threads_main(void);

// Return worker INDEX, or null when INDEX is out of range.
SearchWorker *search_threads_at(size_t index);

ThreadPool *search_threads_pool(void);

// Run `worker_clear` on every worker. Reach this on `ucinewgame`, never per `go`.
void search_threads_clear(void);

// Release the workers, the pool and the history banks.
void search_threads_shutdown(void);

// Start workers 1..N-1 on JOB, each receiving its own SearchWorker as the context, and
// wait for them again. Thread 0 is never started: its caller drives it.
void search_threads_start_siblings(ThreadJobFn job);
void search_threads_wait_siblings(void);

// Return the worker whose root move the pool votes for. Upstream's
// `ThreadPool::get_best_thread` (thread.cpp), reproduced exactly -- including the
// shortest-mate rule and the PV-length tie-break. With one worker it returns that
// worker, which is what keeps a one-thread run's bestmove unchanged.
SearchWorker *search_threads_best(void);

#endif  // MCFISH_SEARCH_THREADS_H
