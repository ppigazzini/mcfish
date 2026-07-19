#include "worker_construct.h"

#include "../../platform/memory.h"

#include <stdlib.h>
#include <string.h>

SearchWorker *worker_create(const WorkerCtorInputs *in) {
    // Take the block from the page allocator: it arrives zeroed, and it is the route the
    // engine's big arenas take so this zone never names an OS allocator. Zeroed is the
    // precondition, not a convenience -- see the header.
    SearchWorker *const w = page_alloc(sizeof *w);
    if (w == nullptr)
        return nullptr;

    w->eval_arena = eval_arena_create();
    if (w->eval_arena == nullptr) {
        worker_destroy(w);
        return nullptr;
    }

    // Only thread 0 carries a manager. A sibling reads `manager == nullptr` where
    // upstream calls into a NullSearchManager whose one virtual does nothing.
    if (in->thread_idx == 0) {
        w->manager = calloc(1, sizeof *w->manager);
        if (w->manager == nullptr) {
            worker_destroy(w);
            return nullptr;
        }
        search_manager_clear(w->manager);
    }

    w->hist.shared = in->shared_history;
    w->threads = in->threads;
    w->thread_idx = in->thread_idx;
    w->numa_thread_idx = in->numa_thread_idx;
    w->numa_total = in->numa_total;
    w->numa_access_token = in->numa_access_token;

    worker_clear(w);
    return w;
}

void worker_destroy(SearchWorker *w) {
    if (w == nullptr)
        return;
    root_moves_free(&w->rml);
    eval_arena_destroy(w->eval_arena);
    free(w->manager);
    page_free(w);
}

bool worker_ensure_network(SearchWorker *w) {
    const uint64_t generation = eval_network_generation();
    if (generation == 0)
        return false;  // no net resident; the classical fallback needs no cache
    if (generation == w->network_generation)
        return true;

    eval_arena_clear_refresh_cache(w->eval_arena);
    w->network_generation = generation;
    return true;
}

void worker_clear(SearchWorker *w) {
    history_clear(&w->hist, w->numa_thread_idx, w->numa_total);
    worker_ensure_network(w);
    if (w->manager != nullptr)
        search_manager_clear(w->manager);
}
