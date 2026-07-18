#include "shared_state.h"

#include "worker_layout.h"

void shared_state_init(SharedState *ss,
                       ThreadPool *threads,
                       TranspositionTable *tt,
                       SharedHistories *shared_histories) {
    ss->threads = threads;
    ss->tt = tt;
    ss->shared_histories = shared_histories;
}

void shared_state_set_stop(SharedState *ss, bool value) {
    thread_pool_set_stop(ss->threads, value);
}

bool shared_state_stopped(const SharedState *ss) { return thread_pool_stopped(ss->threads); }

void shared_state_set_increase_depth(SharedState *ss, bool value) {
    thread_pool_set_increase_depth(ss->threads, value);
}

bool shared_state_increase_depth(const SharedState *ss) {
    return thread_pool_increase_depth(ss->threads);
}

uint64_t shared_state_nodes_searched(const SharedState *ss) {
    return worker_pool_nodes_searched(ss->threads);
}

uint64_t shared_state_tb_hits(const SharedState *ss) { return worker_pool_tb_hits(ss->threads); }
