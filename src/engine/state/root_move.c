#include "root_move.h"

#include "../../platform/memory.h"

#include <string.h>

void pv_moves_clear(PVMoves *pv) { pv->length = 0; }

void pv_moves_push_back(PVMoves *pv, Move m) {
    if (pv->length >= PV_MOVES_CAPACITY)
        return;
    pv->moves[pv->length] = m;
    pv->length += 1;
}

void pv_moves_resize(PVMoves *pv, size_t new_size) {
    if (new_size <= pv->length)
        pv->length = new_size;
}

void pv_moves_update(PVMoves *pv, Move m, const PVMoves *child) {
    // Copy the child line up one slot, then write M at the front. The child cannot be
    // this PV itself: the search always updates a parent from its own child frame.
    pv->length = child != nullptr ? child->length : 0;
    if (pv->length > MAX_PLY)
        pv->length = MAX_PLY;
    if (child != nullptr && pv->length > 0)
        memcpy(pv->moves + 1, child->moves, pv->length * sizeof(Move));
    pv->moves[0] = m;
    pv->length += 1;
}

void root_move_init(RootMove *rm, Move m) {
    rm->effort = 0;
    rm->score = -VALUE_INFINITE;
    rm->previous_score = -VALUE_INFINITE;
    rm->average_score = -VALUE_INFINITE;
    // Keep the product in the int32 domain: 32001 * 32001 fits, and upstream negates the
    // positive product rather than squaring a negative one.
    rm->mean_squared_score = -(VALUE_INFINITE * VALUE_INFINITE);
    rm->uci_score = -VALUE_INFINITE;
    rm->score_lowerbound = false;
    rm->score_upperbound = false;
    rm->previous_score_exact = false;
    rm->sel_depth = 0;
    rm->tb_rank = 0;
    rm->tb_score = 0;
    pv_moves_clear(&rm->pv);
    pv_moves_clear(&rm->previous_pv);
    pv_moves_push_back(&rm->pv, m);
}

bool root_move_less(const RootMove *a, const RootMove *b) {
    return b->score != a->score ? b->score < a->score : b->previous_score < a->previous_score;
}

bool root_move_list_init(RootMoveList *list) {
    list->moves = nullptr;
    list->count = 0;
    list->capacity = 0;

    RootMove *buf = (RootMove *) page_alloc(MAX_MOVES * sizeof(RootMove));
    if (buf == nullptr)
        return false;

    list->moves = buf;
    list->capacity = MAX_MOVES;
    return true;
}

void root_move_list_free(RootMoveList *list) {
    page_free(list->moves);
    list->moves = nullptr;
    list->count = 0;
    list->capacity = 0;
}

void root_move_list_clear(RootMoveList *list) { list->count = 0; }

RootMove *root_move_list_push(RootMoveList *list, Move m) {
    if (list->count >= list->capacity)
        return nullptr;
    RootMove *rm = &list->moves[list->count];
    root_move_init(rm, m);
    list->count += 1;
    return rm;
}

RootMove *root_move_list_find(RootMoveList *list, Move m) {
    for (size_t i = 0; i < list->count; ++i)
        if (root_move_eq(&list->moves[i], m))
            return &list->moves[i];
    return nullptr;
}

// Sort by insertion, which is stable and therefore reproduces std::stable_sort's unique
// answer for this strict weak ordering. The root list is bounded by MAX_MOVES, so the
// quadratic worst case is irrelevant next to the determinism.
void root_move_list_sort(RootMoveList *list, size_t first, size_t last) {
    if (last > list->count)
        last = list->count;
    if (first >= last)
        return;

    for (size_t i = first + 1; i < last; ++i) {
        RootMove key = list->moves[i];
        size_t j = i;
        while (j > first && root_move_less(&key, &list->moves[j - 1])) {
            list->moves[j] = list->moves[j - 1];
            --j;
        }
        list->moves[j] = key;
    }
}
