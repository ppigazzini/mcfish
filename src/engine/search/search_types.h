// Own the search zone's plain-data types: the per-ply Stack, the root-move list,
// the node-type tag, and the SearchCtx every node body threads through.
//
// The invariant the recursion rests on: `Stack` is an array element, never an
// object. Every helper reaches its neighbours by pointer arithmetic — `ss - 1`,
// `ss + 2`, `ss - 6` — so the array MUST carry 7 sentinel frames below the root
// and 2 above MAX_PLY, and every sentinel must be zeroed with a non-null
// continuation page. A frame reached by arithmetic is always assumed live.
//
// Ported from zfish `engine/search/search_types.zig` + `search_ctx.zig` and
// `engine/state/root_move.zig`. Golden: `Stockfish/src/search.h`.

#ifndef CCFISH_SEARCH_TYPES_H
#define CCFISH_SEARCH_TYPES_H

#include "history.h"
#include "timeman.h"

#include "../board/position.h"
#include "../board/types.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

// Name the three node kinds upstream specialises `search<>` on. The bodies take
// this tag and derive `pv_node` / `root_node` from it, so the two flags cannot
// drift apart the way two independent bools can.
typedef enum : uint8_t {
    NT_NON_PV = 0,
    NT_PV = 1,
    NT_ROOT = 2,
} NodeType;

static inline bool nt_is_pv(NodeType nt) { return nt != NT_NON_PV; }
static inline bool nt_is_root(NodeType nt) { return nt == NT_ROOT; }

// Hold one principal variation: a fixed buffer plus a length, never a pointer.
// Copying a PVMoves by assignment is how the root move list saves and restores
// a line, so it must stay trivially copyable.
typedef struct {
    Move moves[MAX_PLY + 1];
    size_t length;
} PVMoves;

// Carry one root move's search record. `score` is this iteration's, `uci_score`
// is what the info line prints (clamped to the aspiration bound on a fail),
// and `previous_*` is the completed previous iteration's — the follow-PV
// heuristic reads `previous_pv`, which is per-line and NOT rootMoves[0]'s.
typedef struct {
    uint64_t effort;
    int32_t score;
    int32_t previous_score;
    int32_t average_score;
    int32_t mean_squared_score;
    int32_t uci_score;
    bool score_lowerbound;
    bool score_upperbound;
    int32_t sel_depth;
    int32_t tb_rank;
    int32_t tb_score;
    PVMoves pv;
    PVMoves previous_pv;
    bool previous_score_exact;
} RootMove;

static inline bool root_move_score_is_bound(const RootMove *rm) {
    return rm->score_lowerbound || rm->score_upperbound;
}

static inline void root_move_unset_bound_flags(RootMove *rm) {
    rm->score_lowerbound = false;
    rm->score_upperbound = false;
}

// Report an exact (non-bound) proven loss. Take IS_LOSS as an argument: the loss
// threshold belongs to the value model, and this type stays free of it
// (zfish root_move.zig: scoreIsExactLoss, upstream search.h:131).
static inline bool root_move_score_is_exact_loss(const RootMove *rm, bool is_loss) {
    return rm->score != -VALUE_INFINITE && is_loss && !root_move_score_is_bound(rm);
}

// Mirror upstream's per-ply Search::Stack. `pv` is null on every frame that did
// not run a PV search; `continuation_history` and
// `continuation_correction_history` are never null, sentinels included.
typedef struct {
    PVMoves *pv;
    int16_t *continuation_history;
    int16_t *continuation_correction_history;
    int32_t ply;
    Move current_move;
    Move excluded_move;
    int32_t static_eval;
    int32_t stat_score;
    int32_t move_count;
    bool in_check;
    bool tt_pv;
    bool tt_hit;
    bool follow_pv;
    int32_t cutoff_cnt;
    int32_t reduction;
} Stack;

// Pad the stack with 7 frames below the root and 2 above MAX_PLY. `ss - 6` is
// read by the continuation walk, `ss + 2` by the cutoff-count reset.
enum { STACK_PAD = 7, STACK_SIZE = STACK_PAD + MAX_PLY + 10 };

// Carry the Syzygy settings the root ranking resolved. `cardinality` is 0 in a
// build with no SyzygyPath, which is what keeps the in-search Step 6 probe out
// of the default node count entirely.
typedef struct {
    int32_t cardinality;
    bool root_in_tb;
    bool use_rule50;
    int32_t probe_depth;
} TbConfig;

// Mirror upstream's LimitsType. `search.h`'s SearchLimits is the shell-facing
// subset; this is the field set the ported search actually reads.
typedef struct {
    TimePoint time[COLOR_NB];
    TimePoint inc[COLOR_NB];
    TimePoint npmsec;
    TimePoint movetime;
    TimePoint start_time;
    int32_t movestogo;
    int32_t depth;
    int32_t mate;
    int32_t perft;
    bool infinite;
    uint64_t nodes;
    bool ponder;
} SearchZoneLimits;

// Carry what check_time reads. Every pointer is null on a non-main thread, where
// check_time returns before touching any of them.
typedef struct {
    int *calls_cnt;
    atomic_bool *stop_write;
    atomic_bool *ponder;
    bool *stop_on_ponderhit;
    TimePoint tm_start_time;
    TimePoint tm_maximum_time;
    uint64_t lim_nodes;
    TimePoint lim_movetime;
    bool tm_use_nodes_time;
    bool use_time_management;
} SearchTimeState;

// Carry the hot per-node context through the whole recursion. Single-threaded,
// so the fields upstream reaches through a Worker pointer live here by value;
// only `stop` stays a pointer, because the input thread writes it.
//
// `reductions` is filled once per search by search_common_fill_reductions and is
// read-only for the duration of the tree.
typedef struct SearchCtx {
    Histories *hist;
    Position *root_pos;

    uint64_t nodes;
    uint64_t tb_hits;
    uint64_t best_move_changes;

    int32_t optimism[COLOR_NB];
    int32_t nmp_min_ply;
    int32_t sel_depth;
    int32_t root_depth;
    int32_t root_delta;

    int32_t reductions[MAX_MOVES];

    PVMoves last_iter_pv;

    atomic_bool *stop;

    size_t pv_idx;
    size_t pv_last;
    RootMove *root_moves;
    size_t root_moves_count;

    TbConfig tb_config;
    SearchZoneLimits limits;
    SearchTimeState time_state;
} SearchCtx;

// Snapshot the iterative-deepening scalars once at entry, as upstream reads them
// off the Worker and its SearchManager. Held separately from SearchCtx because
// none of it is read inside the recursion.
typedef struct {
    TimeManagement *tm;
    bool *stop_on_ponderhit;
    atomic_bool *ponder;
    atomic_bool *increase_depth;
    int32_t iter_value[4];
    double previous_time_reduction;
    double skill_level;

    size_t thread_idx;
    size_t threads_size;
    size_t multipv_option;

    TimePoint tm_optimum;
    TimePoint tm_maximum;
    TimePoint tm_start_time;
    bool tm_use_nodes_time;

    int32_t limits_depth;
    int32_t limits_mate;
    int32_t best_previous_score;
    int32_t best_previous_average_score;

    bool is_main;
    bool use_time_management;
    bool skill_enabled;
} SearchIdState;

#endif  // CCFISH_SEARCH_TYPES_H
