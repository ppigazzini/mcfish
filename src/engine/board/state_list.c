#include "state_list.h"

#include <stdlib.h>
#include <string.h>

// Hold one pointer per StateInfo, never the records themselves. Growing this
// vector moves the POINTERS; the records it points at stay put, which is the
// stability the chain rests on.
struct StateList {
    StateInfo **blocks;
    size_t len;
    size_t capacity;
};

struct PendingStateStorage {
    StateList *list;  // nullptr once moved out
};

// Append one zeroed StateInfo. Free the record if the vector growth fails, since
// it is not tracked yet and destroy would not see it.
static StateInfo *append_block(StateList *list) {
    StateInfo *const block = calloc(1, sizeof(StateInfo));
    if (block == nullptr)
        return nullptr;

    if (list->len == list->capacity) {
        const size_t next = list->capacity ? list->capacity * 2 : 8;
        StateInfo **const grown = realloc(list->blocks, next * sizeof(StateInfo *));
        if (grown == nullptr) {
            free(block);
            return nullptr;
        }
        list->blocks = grown;
        list->capacity = next;
    }

    list->blocks[list->len++] = block;
    return block;
}

StateList *state_list_create(void) {
    StateList *const list = calloc(1, sizeof(StateList));
    if (list == nullptr)
        return nullptr;

    if (append_block(list) == nullptr) {
        state_list_destroy(list);
        return nullptr;
    }
    return list;
}

void state_list_destroy(StateList *list) {
    if (list == nullptr)
        return;

    for (size_t i = 0; i < list->len; ++i)
        free(list->blocks[i]);
    free(list->blocks);
    free(list);
}

StateInfo *state_list_reset(StateList *list) {
    // Reuse the root block rather than reallocating it, so reset cannot fail once
    // the list exists.
    for (size_t i = 1; i < list->len; ++i)
        free(list->blocks[i]);
    list->len = 1;
    memset(list->blocks[0], 0, sizeof(StateInfo));
    return list->blocks[0];
}

StateInfo *state_list_push(StateList *list) { return append_block(list); }

StateInfo *state_list_back(const StateList *list) { return list->blocks[list->len - 1]; }

bool state_list_has_states(const StateList *list) { return list->len != 0; }

size_t state_list_len(const StateList *list) { return list->len; }

PendingStateStorage *pending_states_create(void) {
    PendingStateStorage *const storage = calloc(1, sizeof(PendingStateStorage));
    if (storage == nullptr)
        return nullptr;

    storage->list = state_list_create();
    if (storage->list == nullptr) {
        free(storage);
        return nullptr;
    }
    return storage;
}

void pending_states_destroy(PendingStateStorage *storage) {
    if (storage == nullptr)
        return;

    state_list_destroy(storage->list);
    free(storage);
}

StateInfo *pending_states_reset(PendingStateStorage *storage) {
    if (storage->list == nullptr) {
        storage->list = state_list_create();
        if (storage->list == nullptr)
            return nullptr;
        return state_list_back(storage->list);
    }
    return state_list_reset(storage->list);
}

StateInfo *pending_states_push(PendingStateStorage *storage) {
    return storage->list ? state_list_push(storage->list) : nullptr;
}

bool pending_states_has_states(const PendingStateStorage *storage) {
    return storage->list != nullptr && state_list_has_states(storage->list);
}

StateList *pending_states_move_out(PendingStateStorage *storage) {
    StateList *const list = storage->list;
    storage->list = nullptr;
    return list;
}
