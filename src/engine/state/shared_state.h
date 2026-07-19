// Own the bundle of references the engine hands every Worker at construction: the thread
// pool, the transposition table, and the node's shared histories.
//
// The bundle holds REFERENCES, never ownership. Each referent outlives every worker built
// from it, which is what lets a Worker bind them once at construction and never re-check.
// Aggregating the pool's counters lives here too, so the search reports totals without
// naming the thread module.
//
// The stop flag is the pool's, not a second copy. One flag with one writer and many
// relaxed readers is the whole cross-thread protocol; a mirrored copy is how the siblings
// come to disagree about whether a search is still running.
//
// Upstream: search.h:187 (SharedState).

#ifndef MCFISH_SHARED_STATE_H
#define MCFISH_SHARED_STATE_H

#include "../../platform/thread_pool.h"
#include "../search/history.h"
#include "tt_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    ThreadPool *threads;
    TranspositionTable *tt;
    SharedHistories *shared_histories;
} SharedState;

// Bind the three live references. Upstream also passes the options map and the network;
// both are read through their own globals here, so neither is carried.
void shared_state_init(SharedState *ss,
                       ThreadPool *threads,
                       TranspositionTable *tt,
                       SharedHistories *shared_histories);

// Raise or clear the pool's stop flag. Every worker polls it between nodes.
void shared_state_set_stop(SharedState *ss, bool value);
bool shared_state_stopped(const SharedState *ss);

void shared_state_set_increase_depth(SharedState *ss, bool value);
bool shared_state_increase_depth(const SharedState *ss);

// Sum the counter over every worker in the pool, for `info nodes` / `info tbhits`.
uint64_t shared_state_nodes_searched(const SharedState *ss);
uint64_t shared_state_tb_hits(const SharedState *ss);

#endif  // MCFISH_SHARED_STATE_H
