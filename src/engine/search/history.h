// Own the search history tables and the writes that age, clear and update them.
//
// The tables are the whole move-ordering memory: butterfly (main), low-ply,
// capture, continuation, continuation-correction, and the two key-indexed shared
// tables (pawn, correction). They are one flat `Histories` block so a future
// per-worker port is a second instance, not a re-shape.
//
// The invariant the code cannot show: every entry is a gravity-bounded int16.
// `stats_update` never lets |entry| exceed the D it is called with, so the
// int16 narrowing at the end of it cannot lose a bit for any D used here
// (max 30000). Change a D above 32767 and the narrowing silently wraps.
//
// The updates read the search stack. history.c must not depend on search.c's
// `Stack` layout, so the caller gathers the fields it needs into `HistoryStack`
// before each call: `frames[k]` is `(ss - 1 - k)`, so `frames` is the walk from
// `ss` and `frames + 1` is the walk from `ss - 1`.
//
// Golden: the upstream `history.h` and `search.cpp` (update_all_stats /
// update_continuation_histories / update_quiet_histories /
// update_correction_history).

#ifndef MCFISH_HISTORY_H
#define MCFISH_HISTORY_H

#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>
#include <stdint.h>

enum {
    HIST_UINT16 = 65536,                   // UINT_16_HISTORY_SIZE: one row per raw move word
    LOW_PLY_HISTORY_SIZE = 5,              // LOW_PLY_HISTORY_SIZE
    HIST_PIECE_TYPE_NB = 8,                // capture history's third index is 8-wide, not 7
    HIST_PIECETO = PIECE_NB * SQUARE_NB,   // one PieceToHistory page, indexed pc * 64 + to
    CORRECTION_HISTORY_BASE_SIZE = 65536,  // CORRHIST_BASE_SIZE, the size at ONE thread
    PAWN_HISTORY_BASE_SIZE = 8192,         // PAWN_HISTORY_BASE_SIZE, the size at ONE thread
    CORRECTION_HISTORY_LIMIT = 1024,

    // Hold the continuation block's page count: continuationHistory[in_check][capture].
    CONTINUATION_PAGES = 2 * 2 * HIST_PIECETO,
};

// State the cleared values of the two key-indexed tables once, here, because three places
// clear them: history_clear, the striped shared clear a worker runs for its own slice, and
// the bundle-level clear. NEITHER IS ZERO, so any of the three that reaches for memset is
// silently wrong -- the search reads the correction table on the first node after a clear
// and reads a different value than upstream from then on. Upstream: search.cpp:680-681.
enum : int16_t {
    CORRECTION_HISTORY_FILL = -5,
    PAWN_HISTORY_FILL = -1338,
};

// Bundle the four correction entries stored per (key, color) slot.
typedef struct {
    int16_t pawn;
    int16_t minor;
    int16_t nonpawn_white;
    int16_t nonpawn_black;
} CorrectionBundle;

// Hold the tables the workers of ONE NUMA node share: the key-indexed correction and
// pawn tables, whose sizes scale with that node's thread count, and the continuation
// block, whose size does not. Upstream keeps all three in `SharedHistories` and hands
// every Worker a reference (search.h:341); only the tables above stay per-worker.
//
// The two masks are the sizes minus one and the sizes are powers of two, so an index is
// a mask and never a modulo. Binding a pointer without its mask is what turns a wrapped
// index into an out-of-range read, so `shared_histories_create` is the only writer.
//
// Upstream: history.h:202 (SharedHistories), history.h:95 (DynStats).
typedef struct SharedHistories {
    size_t corr_size;
    size_t corr_size_minus1;
    CorrectionBundle (*correction_history)[COLOR_NB];

    size_t pawn_size;
    size_t pawn_size_minus1;
    int16_t *pawn_history;

    int16_t *continuation_history;  // CONTINUATION_PAGES * HIST_PIECETO entries
} SharedHistories;

// Allocate one node's bank sized for THREAD_COUNT threads, or null. THREAD_COUNT must be
// a power of two and at least 1 -- upstream asserts exactly that (history.h:205), because
// the size multiplier is what keeps the index a mask.
SharedHistories *shared_histories_create(size_t thread_count);
void shared_histories_destroy(SharedHistories *sh);

// Hold one worker's own tables plus the reference to its node's bank. `shared` is never
// null once the worker is constructed; every accessor below dereferences it.
typedef struct {
    int16_t main_history[COLOR_NB * HIST_UINT16];
    int16_t low_ply_history[LOW_PLY_HISTORY_SIZE * HIST_UINT16];
    int16_t capture_history[PIECE_NB * SQUARE_NB * HIST_PIECE_TYPE_NB];
    int16_t continuation_correction_history[HIST_PIECETO * HIST_PIECETO];
    int16_t tt_move_history;
    SharedHistories *shared;
} Histories;

// Return the engine's single history block, bound to a single-thread shared bank. It is
// the block the netless / headless callers -- the unit tests and any `search_go` driven
// without a pool -- run against. Return null when the bank could not be allocated.
Histories *histories(void);

// Release the process-wide block's shared bank. Call once at process exit; the next
// `histories()` rebuilds it.
void histories_shutdown(void);

// Mirror one search-stack frame. `continuation_history` points at a
// PieceToHistory page (HIST_PIECETO entries, indexed pc * 64 + to) and is never
// null on a frame whose `current_move` is a real move — the null-move and
// iterative-deepening sentinels point at the table base.
typedef struct {
    Move current_move;
    int16_t *continuation_history;
} ContHistFrame;

// Carry the search-stack fields the history writes read. `frames[k]` is
// `(ss - 1 - k)`; seven are needed because update_all_stats walks six frames back
// from `ss` for the quiet updates and six back from `ss - 1` for the prev-square
// update. `cont_corr[0]` is `(ss - 2)`'s and `cont_corr[1]` is `(ss - 4)`'s
// continuation-correction page.
typedef struct {
    ContHistFrame frames[7];
    int16_t *cont_corr[2];
    int ply;              // ss->ply
    bool in_check;        // ss->in_check
    bool prev_in_check;   // (ss - 1)->in_check
    int prev_stat_score;  // (ss - 1)->stat_score
    int prev_move_count;  // (ss - 1)->move_count
    bool prev_tt_hit;     // (ss - 1)->tt_hit
} HistoryStack;

// Carry the move lists and node facts update_all_stats scores from.
typedef struct {
    Move best_move;
    Square prev_sq;  // SQ_NONE when the previous ply made no move
    const Move *quiets;
    size_t n_quiets;
    const Move *captures;
    size_t n_captures;
    int depth;
    Move tt_move;
    bool pv_node;
} HistoryStats;

// Carry the four Zobrist keys the correction tables are indexed by.
typedef struct {
    Key pawn;
    Key minor;
    Key non_pawn[COLOR_NB];
} CorrectionKeys;

// Update ENTRY by gravity toward [-D, D]. D must not exceed INT16_MAX.
static inline void stats_update(int16_t *entry, int bonus, int d) {
    const int clamped = bonus < -d ? -d : (bonus > d ? d : bonus);
    const int val = *entry;
    const int abs_clamped = clamped < 0 ? -clamped : clamped;
    *entry = (int16_t) (val + clamped - val * abs_clamped / d);
}

// Return the continuation-history page do_move installs on the stack:
// continuationHistory[in_check][capture][pc][to]. The null move and the
// iterative-deepening sentinels pass all-zero indices and land on the table base.
static inline int16_t *
cont_hist_page(Histories *h, bool in_check, bool capture, Piece pc, Square to) {
    const size_t block = ((size_t) in_check * 2 + (size_t) capture) * HIST_PIECETO
                       + (size_t) pc * SQUARE_NB + (size_t) to;
    return &h->shared->continuation_history[block * HIST_PIECETO];
}

// Return the continuation-correction page for (pc, to).
static inline int16_t *cont_corr_page(Histories *h, Piece pc, Square to) {
    const size_t block = (size_t) pc * SQUARE_NB + (size_t) to;
    return &h->continuation_correction_history[block * HIST_PIECETO];
}

// Return the pawn-history page for PAWN_KEY: HIST_PIECETO entries, pc * 64 + to.
static inline int16_t *pawn_history_row(Histories *h, Key pawn_key) {
    const size_t idx = (size_t) pawn_key & h->shared->pawn_size_minus1;
    return &h->shared->pawn_history[idx * HIST_PIECETO];
}

// Return correctionHistory[key & mask][us].
static inline CorrectionBundle *corr_bundle(Histories *h, Key key, Color us) {
    const size_t idx = (size_t) key & h->shared->corr_size_minus1;
    return &h->shared->correction_history[idx][us];
}

// Return &captureHistory[pc][to][captured_pt].
static inline int16_t *capture_entry(Histories *h, Piece pc, Square to, PieceType captured) {
    return &h->capture_history[(size_t) pc * (SQUARE_NB * HIST_PIECE_TYPE_NB)
                               + (size_t) to * HIST_PIECE_TYPE_NB + (size_t) captured];
}

// Reset one worker's tables and ITS STRIPE of the node's shared tables, exactly as
// upstream's Worker::clear does (search.cpp:676). NUMA_THREAD_IDX is this worker's index
// within its NUMA node and NUMA_TOTAL that node's worker count; a NUMA_TOTAL of 0 is read
// as 1. The stripe is `[i * n / total, (i + 1) * n / total)`, so with one worker the slice
// is the whole table -- and with several, every entry falls in exactly one worker's slice.
//
// The shared CONTINUATION block is deliberately NOT striped: upstream has every worker
// fill all of it (search.cpp:690-694), so the clear is a benign write race by design and
// striping it would be a divergence, not a fix.
//
// Call once per `ucinewgame`, on every worker.
void history_clear(Histories *h, size_t numa_thread_idx, size_t numa_total);

// Decay the main history once per iterative-deepening iteration: v * 729 / 1024.
void history_age_main(Histories *h);

// Refill the low-ply history with 100, once per search.
void history_fill_low_ply(Histories *h);

// Apply BONUS to the main / low-ply / continuation / pawn histories for MOVE.
void history_update_quiet(
  Histories *h, const Position *pos, Key pawn_key, const HistoryStack *hs, Move move, int bonus);

// Apply BONUS along the continuation histories of FRAMES, where FRAMES[k] is the
// stack entry k + 1 plies back from the walk's base. IN_CHECK is the base frame's.
void history_update_continuation(
  const ContHistFrame *frames, bool in_check, Piece pc, Square to, int bonus);

// Reward BEST_MOVE and punish the searched-but-refuted moves after a fail high.
void history_update_all_stats(
  Histories *h, const Position *pos, Key pawn_key, const HistoryStack *hs, const HistoryStats *st);

// Nudge the four key-indexed correction tables and the (ss-2)/(ss-4) continuation
// corrections toward the search / static-eval delta.
void history_update_correction(Histories *h,
                               const Position *pos,
                               Color us,
                               const CorrectionKeys *keys,
                               const HistoryStack *hs,
                               int bonus);

#endif  // MCFISH_HISTORY_H
