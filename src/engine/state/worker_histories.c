#include "worker_histories.h"

#include "correction_bundle.h"

#include <stdint.h>

// Return the half-open stripe [lo, hi) of N entries this worker clears. Divide by
// multiplication first so consecutive workers' stripes abut exactly and every entry
// falls in exactly one of them, whatever N and TOTAL are.
static void stripe(size_t n, size_t thread_idx, size_t total, size_t *lo, size_t *hi) {
    if (total == 0)
        total = 1;
    if (thread_idx >= total) {
        *lo = 0;
        *hi = 0;
        return;
    }
    *lo = n * thread_idx / total;
    *hi = n * (thread_idx + 1) / total;
}

void worker_histories_bind_shared(WorkerHistories *wh, SharedHistories *sh) {
    wh->shared_history = sh;
}

void worker_histories_bind_own(WorkerHistories *wh, SharedHistories *sh) {
    shared_histories_bind(sh, &wh->tables);
    wh->shared_history = sh;
}

void worker_histories_clear(WorkerHistories *wh) { history_clear(&wh->tables); }

void worker_histories_clear_shared(SharedHistories *sh, size_t thread_idx, size_t total) {
    if (sh == nullptr)
        return;

    if (sh->corr_data != nullptr) {
        size_t lo, hi;
        stripe(sh->corr_size, thread_idx, total, &lo, &hi);
        // Write the entries of each [COLOR_NB] page as one contiguous int16 run: the
        // bundle is four contiguous entries, so the page is COLOR_NB * 4 of them.
        for (size_t i = lo; i < hi; ++i) {
            int16_t *page = (int16_t *) &sh->corr_data[i][0];
            const size_t entries = (size_t) COLOR_NB * (size_t) CORR_FIELD_NB;
            for (size_t k = 0; k < entries; ++k)
                page[k] = CORRECTION_HISTORY_FILL;
        }
    }

    if (sh->pawn_data != nullptr) {
        size_t lo, hi;
        stripe(sh->pawn_size, thread_idx, total, &lo, &hi);
        for (size_t i = lo * HIST_PIECETO; i < hi * HIST_PIECETO; ++i)
            sh->pawn_data[i] = PAWN_HISTORY_FILL;
    }
}
