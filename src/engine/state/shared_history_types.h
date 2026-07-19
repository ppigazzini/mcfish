// Own the reference every Worker holds to the key-indexed history tables it does NOT
// own: the correction table and the pawn table.
//
// These two are shared per NUMA node while the butterfly / capture / continuation tables
// are per-worker, because they are indexed by a Zobrist key rather than by a move and are
// sized in proportion to the thread count. The reference carries the size AND the
// index mask together; the mask is `size - 1` and the size is a power of two, so an index
// is a mask and never a modulo. Binding one without the other is what turns a wrapped
// index into an out-of-range read.
//
// The block itself is owned elsewhere -- `shared_histories_bind` points this at the
// correction and pawn arrays inside an existing `Histories`, so nothing here allocates.
//
// Upstream: search.h (SharedHistories), history.h (CorrectionHistory, PawnHistory).

#ifndef MCFISH_SHARED_HISTORY_TYPES_H
#define MCFISH_SHARED_HISTORY_TYPES_H

#include "../board/types.h"
#include "../search/history.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Point at correction_history[corr_size][COLOR_NB].
    size_t corr_size;
    CorrectionBundle (*corr_data)[COLOR_NB];

    // Point at pawn_history[pawn_size][HIST_PIECETO], flattened.
    size_t pawn_size;
    int16_t *pawn_data;

    // Mask an index into each table. Both are the table size minus one.
    size_t size_minus1;
    size_t pawn_hist_size_minus1;
} SharedHistories;

// Point SH at the two key-indexed tables inside H and derive both masks. H stays the
// owner; SH is a reference and must not outlive it.
static inline void shared_histories_bind(SharedHistories *sh, Histories *h) {
    sh->corr_size = CORRECTION_HISTORY_SIZE;
    sh->corr_data = h->correction_history;
    sh->pawn_size = PAWN_HISTORY_SIZE;
    sh->pawn_data = h->pawn_history;
    sh->size_minus1 = CORRECTION_HISTORY_SIZE - 1;
    sh->pawn_hist_size_minus1 = PAWN_HISTORY_SIZE - 1;
}

// Return correctionHistory[key & mask][us].
static inline CorrectionBundle *
shared_histories_bundle(const SharedHistories *sh, Key key, Color us) {
    return &sh->corr_data[(size_t) key & sh->size_minus1][us];
}

// Return the pawn-history row for PAWN_KEY: HIST_PIECETO entries, indexed pc * 64 + to.
static inline int16_t *shared_histories_pawn_row(const SharedHistories *sh, Key pawn_key) {
    return &sh->pawn_data[((size_t) pawn_key & sh->pawn_hist_size_minus1) * HIST_PIECETO];
}

#endif  // MCFISH_SHARED_HISTORY_TYPES_H
