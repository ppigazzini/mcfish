#include "worker_construct.h"

#include "../eval/nnue/nnue_accumulator.h"
#include "../eval/nnue/nnue_ft.h"
#include "../eval/nnue/nnue_weight_storage.h"

#include <math.h>
#include <string.h>

void worker_ctor_inputs_from_shared(WorkerCtorInputs *in,
                                    const SharedState *ss,
                                    SearchManager *manager,
                                    size_t thread_idx,
                                    size_t numa_thread_idx,
                                    size_t numa_total,
                                    size_t numa_access_token) {
    in->shared_history = ss->shared_histories;
    in->threads = ss->threads;
    in->tt = ss->tt;
    in->manager = manager;
    in->thread_idx = thread_idx;
    in->numa_thread_idx = numa_thread_idx;
    in->numa_total = numa_total;
    in->numa_access_token = numa_access_token;
}

void worker_write_constructor_fields(Worker *w, const WorkerCtorInputs *in) {
    worker_histories_bind_shared(&w->histories, in->shared_history);

    w->threads = in->threads;
    w->tt = in->tt;
    w->manager = in->manager;

    w->thread_idx = in->thread_idx;
    w->numa_thread_idx = in->numa_thread_idx;
    w->numa_total = in->numa_total;
    w->numa_access_token = in->numa_access_token;

    atomic_u64_init(&w->nodes, 0);
    atomic_u64_init(&w->tb_hits, 0);
    atomic_u64_init(&w->best_move_changes, 0);

    w->limits = limits_type_default();

    // Initialise the tablebase config HERE, not by relying on a zeroed block. Upstream's
    // Tablebases::Config carries its own in-class initialisers (tbprobe.h:41), so a Worker
    // that never runs a root setup still reads cardinality 0 and never probes; mcfish's
    // TablebaseConfig is a plain struct with none, so without this line a worker built
    // before the first `go` reads a stale cardinality and probes a tablebase the root
    // ranking never resolved.
    worker_set_tb_config(w, 0, false, false, 0);

    // Start the accumulator stack with one live, uncomputed root slot.
    nnue_acc_stack_reset(w->accumulator_stack);
}

void worker_fill_reductions(int *reductions, size_t count) {
    if (count == 0)
        return;
    reductions[0] = 0;
    for (size_t i = 1; i < count; ++i) {
        // Keep the whole expression in double and truncate toward zero exactly once, as
        // upstream's int(2834 / 128.0 * std::log(i)) does. Re-associating the constants
        // moves a rounding boundary and with it the node count.
        const double logv = log((double) i);
        reductions[i] = (int) (2834.0 / 128.0 * logv);
    }
}

void worker_clear(Worker *w) {
    worker_histories_clear(&w->histories);
    worker_histories_clear_shared(w->histories.shared_history, w->numa_thread_idx, w->numa_total);
    worker_fill_reductions(w->reductions, MAX_MOVES);
    nnue_acc_stack_reset(w->accumulator_stack);
}

bool worker_seed_refresh_cache(Worker *w) {
    const uint8_t *ft_bytes = nnue_ft_ptr();
    if (ft_bytes == nullptr)
        return false;

    const NnueFeatureTransformer *ft = (const NnueFeatureTransformer *) ft_bytes;
    nnue_clear_refresh_cache(w->refresh_table, nnue_ft_biases(ft));
    return true;
}

Worker *worker_construct_full(void *block, const WorkerCtorInputs *in, bool *network_ready) {
    if (block == nullptr)
        return nullptr;

    // Zero the whole block first. The allocator hands it over uninitialised, and a reused
    // block otherwise carries the previous worker's root position and PV buffers into the
    // fields the ID loop writes only once it has a root -- so a `go` on a position with no
    // legal move would report the previous search's line.
    memset(block, 0, worker_block_bytes());

    Worker *w = worker_block_init(block);
    if (!root_move_list_init(&w->root_moves))
        return nullptr;

    worker_write_constructor_fields(w, in);
    worker_clear(w);

    const bool seeded = worker_seed_refresh_cache(w);
    if (network_ready != nullptr)
        *network_ready = seeded;

    return w;
}

void worker_destruct(Worker *w) {
    if (w == nullptr)
        return;
    root_move_list_free(&w->root_moves);
    w->histories.shared_history = nullptr;
    w->threads = nullptr;
    w->tt = nullptr;
    w->manager = nullptr;
}
