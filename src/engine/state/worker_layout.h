// Own the per-worker state block: the history tables, the root position and its move
// list, the search bookkeeping, and the two NNUE arenas.
//
// A Worker is ONE allocation. The fixed struct sits at offset 0 and the accumulator stack
// and refresh cache follow it, each 64-byte aligned, because their sizes are runtime
// values (nnue_accumulator_stack_bytes / nnue_refresh_cache_bytes) and cannot be array
// members. Keeping them in the same block is not a convenience: the block is first-touched
// on the NUMA node its thread will run on, and splitting the arenas out would put the
// accumulator on whichever node happened to allocate it.
//
// THE ALIGNMENT IS A PRECONDITION, NOT A PREFERENCE. The NNUE code reads both arenas with
// aligned vector loads. Allocate the block only through worker_block_alloc, which honours
// WORKER_ALIGN, and bind the arena pointers only through worker_block_init.
//
// Nothing here may make thread 0's behaviour depend on an address, a clock or scheduling.
// `nodes`, `tb_hits` and `best_move_changes` are relaxed atomics because other threads
// READ them for reporting -- no search decision may be taken on another worker's counter.
//
// Upstream: search.h:311 (Worker), search.h:242 (SearchManager), thread.h (ThreadPool).
// Port source: zfish src/engine/state/worker_layout.zig.

#ifndef CCFISH_WORKER_LAYOUT_H
#define CCFISH_WORKER_LAYOUT_H

#include "../../platform/thread_pool.h"
#include "../../platform/thread_runtime.h"
#include "../board/position.h"
#include "../board/types.h"
#include "../eval/nnue/nnue_accumulator.h"
#include "../search/timeman.h"
#include "limits_type.h"
#include "root_move.h"
#include "tt_types.h"
#include "worker_histories.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Align the whole block to a cache line, which is also the NNUE arenas' requirement.
enum { WORKER_ALIGN = 64 };

// Mirror upstream's Tablebases::Config. Kept here until the tablebase zone owns a Config
// of its own -- see PORT_NOTES_state.md.
typedef struct {
    int cardinality;
    bool root_in_tb;
    bool use_rule50;
    int probe_depth;
} TablebaseConfig;

// Carry the callbacks the manager reports through. Opaque here: the state zone must not
// name the shell, and the search installs whatever it was handed.
typedef const void *UpdateContext;

// Hold the main thread's per-search bookkeeping. Only thread 0 has one; the siblings run
// with a null manager, which is upstream's NullSearchManager with the virtual call
// removed.
typedef struct {
    TimeManagement tm;
    double original_time_adjust;
    int calls_cnt;
    // Poll this from the search while the input thread writes it, so the two accesses to
    // the one location are both atomic.
    AtomicBool ponder;
    int iter_value[4];
    double previous_time_reduction;
    Value best_previous_score;
    Value best_previous_average_score;
    bool stop_on_ponderhit;
    size_t id;
    UpdateContext updates;
} SearchManager;

void search_manager_init(SearchManager *sm);

// Reset the per-search fields the pool re-inits at the top of every `go`.
void search_manager_reset_calls_count(SearchManager *sm);
void search_manager_reset_best_previous_score(SearchManager *sm);
void search_manager_reset_best_previous_average_score(SearchManager *sm);
void search_manager_reset_original_time_adjust(SearchManager *sm);
void search_manager_reset_previous_time_reduction(SearchManager *sm);
void search_manager_clear_timeman(SearchManager *sm);

void search_manager_set_ponder(SearchManager *sm, bool value);
bool search_manager_ponder(const SearchManager *sm);
void search_manager_set_stop_on_ponderhit(SearchManager *sm, bool value);

typedef struct Worker {
    WorkerHistories histories;

    LimitsType limits;

    size_t pv_idx;
    size_t pv_last;

    // Publish these three for reporting. Written only by this worker, read by any.
    AtomicU64 nodes;
    AtomicU64 tb_hits;
    AtomicU64 best_move_changes;

    int sel_depth;
    int nmp_min_ply;
    Value optimism[COLOR_NB];

    Position root_pos;
    StateInfo root_state;
    RootMoveList root_moves;
    int root_depth;
    Value root_delta;

    PVMoves last_iteration_pv;

    size_t thread_idx;
    size_t numa_thread_idx;
    size_t numa_total;
    size_t numa_access_token;

    // Index by depth or by move number, both bounded by MAX_MOVES.
    int reductions[MAX_MOVES];

    SearchManager *manager;  // null on every worker but thread 0
    TablebaseConfig tb_config;

    ThreadPool *threads;
    TranspositionTable *tt;

    // Point into this block's trailing arenas. Bound by worker_block_init, never
    // separately allocated.
    NnueAccumulatorStack *accumulator_stack;
    NnueRefreshCache *refresh_table;
} Worker;

// Return the byte size one Worker block needs: the struct, then each arena rounded up to
// WORKER_ALIGN. Call it rather than deriving a size, so an NNUE architecture change moves
// the allocation with it.
size_t worker_block_bytes(void);

// Report where the two arenas sit inside the block. Exposed for the layout tests; the
// construction path uses worker_block_init.
size_t worker_accumulator_stack_offset(void);
size_t worker_refresh_table_offset(void);

// Allocate one zeroed, WORKER_ALIGN-aligned block of worker_block_bytes bytes, or null.
void *worker_block_alloc(void);
void worker_block_free(void *block);

// Bind BLOCK's arena pointers and return it as a Worker. BLOCK must be at least
// worker_block_bytes bytes, WORKER_ALIGN-aligned and zeroed.
Worker *worker_block_init(void *block);

static inline bool worker_is_mainthread(const Worker *w) { return w->thread_idx == 0; }

// Read and write the three published counters.
uint64_t worker_nodes(const Worker *w);
void worker_set_nodes(Worker *w, uint64_t value);
void worker_add_nodes(Worker *w, uint64_t delta);
uint64_t worker_tb_hits(const Worker *w);
void worker_set_tb_hits(Worker *w, uint64_t value);
void worker_add_tb_hits(Worker *w, uint64_t delta);
uint64_t worker_best_move_changes(const Worker *w);
void worker_set_best_move_changes(Worker *w, uint64_t value);

// Zero the per-search counters the pool re-inits before each `go`.
void worker_reset_root_setup_state(Worker *w);

void worker_set_tb_config(
  Worker *w, int cardinality, bool root_in_tb, bool use_rule50, int probe_depth);

// Copy SRC into W's root state as a struct, not as a byte range.
void worker_set_root_state(Worker *w, const StateInfo *src);

static inline Position *worker_root_pos(Worker *w) { return &w->root_pos; }
static inline StateInfo *worker_root_state(Worker *w) { return &w->root_state; }

// Return &root_moves[0], or null when the list is empty.
RootMove *worker_root_moves_first(Worker *w);

// Return the Worker attached to THREAD, or null when nothing is attached.
Worker *worker_from_thread(const Thread *thread);

// Sum a counter over every worker in POOL. The search driver reports through these
// rather than importing the thread module, and must never branch on the result at a
// search node -- with more than one thread the sum is not reproducible.
uint64_t worker_pool_nodes_searched(const ThreadPool *pool);
uint64_t worker_pool_tb_hits(const ThreadPool *pool);

// Return thread 0's SearchManager, or null before the pool is built.
SearchManager *worker_pool_main_manager(const ThreadPool *pool);

#endif  // CCFISH_WORKER_LAYOUT_H
