#include "root_pv.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Grow PV to hold at least WANT moves, doubling rather than fitting exactly. The
// operation that outgrows the reservation is the tablebase walk, which appends one
// move at a time; the other two grow at most once.
static bool reserve(RootPVMoves *pv, size_t want) {
    if (want <= pv->capacity)
        return true;

    size_t cap = pv->capacity != 0 ? pv->capacity : (size_t) ROOT_PV_SEARCH_CAP;
    while (cap < want) {
        // Nothing in the type bounds the DTZ walk, so nothing here may assume the
        // doubling terminates before it wraps. Refuse the step that would.
        if (cap > SIZE_MAX / 2 / sizeof *pv->moves)
            return false;
        cap *= 2;
    }

    Move *const grown = realloc(pv->moves, cap * sizeof *grown);
    if (grown == nullptr)
        return false;
    pv->moves = grown;
    pv->capacity = cap;
    return true;
}

bool root_pv_init(RootPVMoves *pv) {
    pv->moves = malloc((size_t) ROOT_PV_SEARCH_CAP * sizeof *pv->moves);
    pv->length = 0;
    pv->capacity = pv->moves != nullptr ? (size_t) ROOT_PV_SEARCH_CAP : 0;
    return pv->moves != nullptr;
}

void root_pv_release(RootPVMoves *pv) {
    free(pv->moves);
    pv->moves = nullptr;
    pv->length = 0;
    pv->capacity = 0;
}

void root_pv_set_line(RootPVMoves *pv, Move first, const Move *child, size_t len) {
    // Reserve rather than assert: 1 + LEN is inside ROOT_PV_SEARCH_CAP on every
    // call the search makes, so this returns without allocating, and a PV whose
    // reservation failed earlier heals here instead of staying unwritable.
    if (!reserve(pv, len + 1)) {
        pv->length = 0;
        return;
    }
    pv->moves[0] = first;
    // memcpy's operands are declared nonnull, so a zero-length copy from a null
    // CHILD is undefined even though it moves no byte.
    if (len != 0)
        memcpy(pv->moves + 1, child, len * sizeof *pv->moves);
    pv->length = 1 + len;
}

bool root_pv_push(RootPVMoves *pv, Move m) {
    if (!reserve(pv, pv->length + 1))
        return false;
    pv->moves[pv->length++] = m;
    return true;
}

void root_pv_truncate(RootPVMoves *pv, size_t len) {
    if (len < pv->length)
        pv->length = len;
}

void root_pv_copy(RootPVMoves *dst, const RootPVMoves *src) {
    if (dst == src)
        return;
    // A failed reserve means the source outran what DST holds, since reserve
    // succeeds whenever the length already fits.
    size_t len = src->length;
    if (!reserve(dst, len))
        len = dst->capacity;
    if (len != 0)
        memcpy(dst->moves, src->moves, len * sizeof *dst->moves);
    dst->length = len;
}
