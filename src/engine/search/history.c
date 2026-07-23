#include "history.h"

#include "../../platform/memory.h"
#include "../board/position.h"
#include "../board/types.h"

#include <stdint.h>

SharedHistories *shared_histories_create(size_t thread_count) {
    if (thread_count == 0 || (thread_count & (thread_count - 1)) != 0)
        return nullptr;

    SharedHistories *sh = page_alloc(sizeof *sh);
    if (sh == nullptr)
        return nullptr;

    // Scale both key-indexed tables by the node's thread count, as upstream's DynStats
    // does (history.h:97, `size = s * SizeMultiplier`). The continuation block does not
    // scale -- it is indexed by a move, not by a Zobrist key.
    sh->corr_size = thread_count * (size_t) CORRECTION_HISTORY_BASE_SIZE;
    sh->corr_size_minus1 = sh->corr_size - 1;
    sh->pawn_size = thread_count * (size_t) PAWN_HISTORY_BASE_SIZE;
    sh->pawn_size_minus1 = sh->pawn_size - 1;

    sh->correction_history = page_alloc(sh->corr_size * COLOR_NB * sizeof(CorrectionBundle));
    sh->pawn_history = page_alloc(sh->pawn_size * HIST_PIECETO * sizeof(int16_t));
    sh->continuation_history =
      page_alloc((size_t) CONTINUATION_PAGES * HIST_PIECETO * sizeof(int16_t));

    if (sh->correction_history == nullptr || sh->pawn_history == nullptr
        || sh->continuation_history == nullptr) {
        shared_histories_destroy(sh);
        return nullptr;
    }
    return sh;
}

void shared_histories_destroy(SharedHistories *sh) {
    if (sh == nullptr)
        return;
    page_free(sh->correction_history);
    page_free(sh->pawn_history);
    page_free(sh->continuation_history);
    page_free(sh);
}

// Hold the block a caller with no pool runs against: the unit tests, the bench harness and
// any direct `search_go`. Built on first use at one thread, so its table sizes -- and
// therefore every index mask the search takes -- are upstream's one-thread sizes.
static Histories Tables;
static SharedHistories *TablesShared = nullptr;

Histories *histories(void) {
    if (TablesShared == nullptr) {
        TablesShared = shared_histories_create(1);
        if (TablesShared == nullptr)
            return nullptr;
        histories_bind_shared(&Tables, TablesShared);
    }
    return &Tables;
}

void histories_shutdown(void) {
    shared_histories_destroy(TablesShared);
    TablesShared = nullptr;
    histories_bind_shared(&Tables, nullptr);
}

// Test move validity as upstream's Move::is_ok does — by the two reserved
// encodings, not by from != to. Any other move with from == to would be a bug
// elsewhere, and treating it as "not ok" here would hide it.
static inline bool is_ok(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

// Return the half-open stripe [*lo, *hi) of N entries worker IDX of TOTAL clears. Divide
// after multiplying so consecutive stripes abut exactly and no entry is missed, whatever
// N and TOTAL are. Upstream: history.h:101 (DynStats::clear_range).
static void stripe(size_t n, size_t idx, size_t total, size_t *lo, size_t *hi) {
    if (total == 0)
        total = 1;
    if (idx >= total) {
        *lo = 0;
        *hi = 0;
        return;
    }
    *lo = n * idx / total;
    *hi = idx + 1 == total ? n : n * (idx + 1) / total;
}

void history_clear(Histories *h, size_t numa_thread_idx, size_t numa_total) {
    // Worker tables: mainHistory=-5, captureHistory=-742, ttMoveHistory=0,
    // continuationCorrectionHistory=5.
    for (size_t i = 0; i < COLOR_NB * HIST_UINT16; ++i)
        h->main_history[i] = -5;
    for (size_t i = 0; i < PIECE_NB * SQUARE_NB * HIST_PIECE_TYPE_NB; ++i)
        h->capture_history[i] = -742;
    h->tt_move_history = 0;
    for (size_t i = 0; i < HIST_PIECETO * HIST_PIECETO; ++i)
        h->continuation_correction_history[i] = 5;

    SharedHistories *const sh = h->shared;

    // Shared continuation block: ONE worker per node fills it, with plain stores.
    //
    // Every entry gets the same value, so a single writer is equivalent to every
    // worker redoing the whole block -- and a non-atomic fill VECTORIZES into
    // broadcast stores, where the `_Atomic` element type cannot: an atomic store is
    // "instruction cannot be vectorized" to the loop vectorizer, so the previous
    // every-worker atomic form was 4.19M scalar stores per worker and the single
    // biggest instruction gap against upstream (183M vs upstream's 67M clear).
    //
    // Race-free without its own barrier: thread_pool_set joins every worker build
    // before any search begins, and this fill runs inside that build, so the block
    // is fully written before any worker reads it. The entries stay `_Atomic` for
    // the concurrent SEARCH; only this exclusive clear phase bypasses it, through
    // the same atomic->plain cast the correction clear below already uses.
    if (numa_thread_idx == 0) {
        int16_t *const cont = (int16_t *) (void *) sh->continuation_history;
        for (size_t i = 0; i < (size_t) CONTINUATION_PAGES * HIST_PIECETO; ++i)
            cont[i] = -586;
    }

    // Shared key-indexed tables, striped. Use the fill constants history.h declares --
    // NEITHER IS ZERO, so a memset here reads back a different value than upstream on the
    // first node after a clear.
    //
    // Each stripe is disjoint across workers, so these fills are race-free even run
    // concurrently, and they go through a plain int16 view -- NOT the `_Atomic`
    // element access -- so the loop vectorizes into broadcast stores. An atomic store
    // does not vectorize, and that alone left this clear ~2.7x upstream's. A
    // CorrectionBundle is COLOR_NB * 4 contiguous int16, so a page range [lo, hi) is
    // the flat int16 range [lo * COLOR_NB * 4, hi * COLOR_NB * 4) -- one contiguous
    // fill, not a length-8 inner loop the cost model refuses to vectorize.
    enum { CORR_INTS_PER_PAGE = COLOR_NB * 4 };
    size_t lo, hi;
    stripe(sh->corr_size, numa_thread_idx, numa_total, &lo, &hi);
    int16_t *const corr = (int16_t *) (void *) sh->correction_history;
    for (size_t i = lo * CORR_INTS_PER_PAGE; i < hi * CORR_INTS_PER_PAGE; ++i)
        corr[i] = CORRECTION_HISTORY_FILL;

    stripe(sh->pawn_size, numa_thread_idx, numa_total, &lo, &hi);
    int16_t *const pawn = (int16_t *) (void *) sh->pawn_history;
    for (size_t i = lo * HIST_PIECETO; i < hi * HIST_PIECETO; ++i)
        pawn[i] = PAWN_HISTORY_FILL;

    // low_ply_history is refilled per search by history_fill_low_ply, never here.
}

void history_age_main(Histories *h) {
    for (size_t i = 0; i < COLOR_NB * HIST_UINT16; ++i) {
        const int v = h->main_history[i];
        h->main_history[i] = (int16_t) (v * 729 / 1024);  // upstream 3c858c19e: drop the +5
    }
}

void history_fill_low_ply(Histories *h) {
    for (size_t i = 0; i < LOW_PLY_HISTORY_SIZE * HIST_UINT16; ++i)
        h->low_ply_history[i] = 102;
}

void history_update_correction(Histories *h,
                               const Position *pos,
                               Color us,
                               const CorrectionKeys *keys,
                               Move prev_move,
                               int16_t *cont_corr2,
                               int16_t *cont_corr4,
                               int bonus) {
    shared_stats_update(&corr_bundle(h, keys->pawn, us)->pawn, bonus, CORRECTION_HISTORY_LIMIT);
    shared_stats_update(&corr_bundle(h, keys->minor, us)->minor, bonus * 150 / 128,
                        CORRECTION_HISTORY_LIMIT);
    shared_stats_update(&corr_bundle(h, keys->non_pawn[WHITE], us)->nonpawn_white,
                        bonus * 186 / 128, CORRECTION_HISTORY_LIMIT);
    shared_stats_update(&corr_bundle(h, keys->non_pawn[BLACK], us)->nonpawn_black,
                        bonus * 186 / 128, CORRECTION_HISTORY_LIMIT);

    if (is_ok(prev_move)) {
        const Square to = move_to(prev_move);
        const size_t idx = (size_t) piece_on(pos, to) * SQUARE_NB + (size_t) to;
        stats_update(&cont_corr2[idx], bonus * 130 / 128, CORRECTION_HISTORY_LIMIT);
        stats_update(&cont_corr4[idx], bonus * 70 / 128, CORRECTION_HISTORY_LIMIT);
    }
}
