#include "worker_layout.h"

#include "../../platform/memory.h"
#include "../../platform/thread.h"

#include <string.h>

static inline size_t round_up(size_t value, size_t align) {
    return (value + align - 1) / align * align;
}

size_t worker_accumulator_stack_offset(void) { return round_up(sizeof(Worker), WORKER_ALIGN); }

size_t worker_refresh_table_offset(void) {
    return worker_accumulator_stack_offset()
         + round_up(nnue_accumulator_stack_bytes(), WORKER_ALIGN);
}

size_t worker_block_bytes(void) {
    return worker_refresh_table_offset() + round_up(nnue_refresh_cache_bytes(), WORKER_ALIGN);
}

void *worker_block_alloc(void) {
    // Take the block from the large-page allocator: it is 2 MiB-aligned, which satisfies
    // WORKER_ALIGN, and it arrives zeroed, which the construction path relies on.
    return aligned_large_pages_alloc(worker_block_bytes());
}

void worker_block_free(void *block) { aligned_large_pages_free(block); }

Worker *worker_block_init(void *block) {
    if (block == nullptr)
        return nullptr;

    unsigned char *bytes = (unsigned char *) block;
    Worker *w = (Worker *) block;
    w->accumulator_stack = (NnueAccumulatorStack *) (bytes + worker_accumulator_stack_offset());
    w->refresh_table = (NnueRefreshCache *) (bytes + worker_refresh_table_offset());
    return w;
}

void search_manager_init(SearchManager *sm) {
    memset(sm, 0, sizeof *sm);
    timeman_clear(&sm->tm);
    atomic_bool_init(&sm->ponder, false);
    search_manager_reset_original_time_adjust(sm);
    search_manager_reset_previous_time_reduction(sm);
    search_manager_reset_best_previous_score(sm);
    search_manager_reset_best_previous_average_score(sm);
}

void search_manager_reset_calls_count(SearchManager *sm) { sm->calls_cnt = 0; }

void search_manager_reset_best_previous_score(SearchManager *sm) {
    sm->best_previous_score = VALUE_INFINITE;
}

void search_manager_reset_best_previous_average_score(SearchManager *sm) {
    sm->best_previous_average_score = VALUE_INFINITE;
}

void search_manager_reset_original_time_adjust(SearchManager *sm) {
    sm->original_time_adjust = -1.0;
}

void search_manager_reset_previous_time_reduction(SearchManager *sm) {
    sm->previous_time_reduction = 0.85;
}

// Mark the `nodes as time` budget unstarted, as TimeManagement::clear does.
void search_manager_clear_timeman(SearchManager *sm) { sm->tm.available_nodes = -1; }

void search_manager_set_ponder(SearchManager *sm, bool value) {
    atomic_bool_store(&sm->ponder, value);
}

bool search_manager_ponder(const SearchManager *sm) { return atomic_bool_load(&sm->ponder); }

void search_manager_set_stop_on_ponderhit(SearchManager *sm, bool value) {
    sm->stop_on_ponderhit = value;
}

uint64_t worker_nodes(const Worker *w) { return atomic_u64_load(&w->nodes); }

void worker_set_nodes(Worker *w, uint64_t value) { atomic_u64_store(&w->nodes, value); }

void worker_add_nodes(Worker *w, uint64_t delta) {
    // Add through a load/store pair rather than a fetch-add: the counter has exactly one
    // writer, so the read-modify-write needs no atomicity, only the store's visibility.
    atomic_u64_store(&w->nodes, atomic_u64_load(&w->nodes) + delta);
}

uint64_t worker_tb_hits(const Worker *w) { return atomic_u64_load(&w->tb_hits); }

void worker_set_tb_hits(Worker *w, uint64_t value) { atomic_u64_store(&w->tb_hits, value); }

void worker_add_tb_hits(Worker *w, uint64_t delta) {
    atomic_u64_store(&w->tb_hits, atomic_u64_load(&w->tb_hits) + delta);
}

uint64_t worker_best_move_changes(const Worker *w) {
    return atomic_u64_load(&w->best_move_changes);
}

void worker_set_best_move_changes(Worker *w, uint64_t value) {
    atomic_u64_store(&w->best_move_changes, value);
}

void worker_reset_root_setup_state(Worker *w) {
    worker_set_nodes(w, 0);
    worker_set_tb_hits(w, 0);
    worker_set_best_move_changes(w, 0);
    w->nmp_min_ply = 0;
    w->root_depth = 0;
}

void worker_set_tb_config(
  Worker *w, int cardinality, bool root_in_tb, bool use_rule50, int probe_depth) {
    w->tb_config.cardinality = cardinality;
    w->tb_config.root_in_tb = root_in_tb;
    w->tb_config.use_rule50 = use_rule50;
    w->tb_config.probe_depth = probe_depth;
}

void worker_set_root_state(Worker *w, const StateInfo *src) { w->root_state = *src; }

RootMove *worker_root_moves_first(Worker *w) {
    return w->root_moves.count > 0 ? &w->root_moves.moves[0] : nullptr;
}

Worker *worker_from_thread(const Thread *thread) {
    return thread != nullptr ? (Worker *) thread_worker(thread) : nullptr;
}

uint64_t worker_pool_nodes_searched(const ThreadPool *pool) {
    uint64_t total = 0;
    const size_t n = thread_pool_num_threads(pool);
    for (size_t i = 0; i < n; ++i) {
        const Worker *w = worker_from_thread(thread_pool_thread_at((ThreadPool *) pool, i));
        if (w != nullptr)
            total += worker_nodes(w);
    }
    return total;
}

uint64_t worker_pool_tb_hits(const ThreadPool *pool) {
    uint64_t total = 0;
    const size_t n = thread_pool_num_threads(pool);
    for (size_t i = 0; i < n; ++i) {
        const Worker *w = worker_from_thread(thread_pool_thread_at((ThreadPool *) pool, i));
        if (w != nullptr)
            total += worker_tb_hits(w);
    }
    return total;
}

SearchManager *worker_pool_main_manager(const ThreadPool *pool) {
    Worker *w = worker_from_thread(thread_pool_thread_at((ThreadPool *) pool, 0));
    return w != nullptr ? w->manager : nullptr;
}
