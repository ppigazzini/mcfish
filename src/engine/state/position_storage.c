#include "position_storage.h"

#include "../../platform/memory.h"

#include <string.h>

static PositionStorageChunk *chunk_new(void) {
    return (PositionStorageChunk *) page_alloc(sizeof(PositionStorageChunk));
}

bool position_storage_init(PositionStorage *ps) {
    memset(ps, 0, sizeof *ps);

    PositionStorageChunk *chunk = chunk_new();
    if (chunk == nullptr)
        return false;

    ps->head = chunk;
    ps->tail = chunk;
    // Hand out the anchor immediately, so position_storage_root is valid from init on.
    ps->used = 1;
    ps->count = 1;
    return true;
}

void position_storage_free(PositionStorage *ps) {
    PositionStorageChunk *chunk = ps->head;
    while (chunk != nullptr) {
        PositionStorageChunk *next = chunk->next;
        page_free(chunk);
        chunk = next;
    }
    memset(ps, 0, sizeof *ps);
}

void position_storage_reset(PositionStorage *ps) {
    if (ps->head == nullptr)
        return;
    memset(&ps->pos, 0, sizeof ps->pos);
    memset(&ps->head->states[0], 0, sizeof ps->head->states[0]);
    ps->tail = ps->head;
    ps->used = 1;
    ps->count = 1;
}

StateInfo *position_storage_root(PositionStorage *ps) {
    return ps->head != nullptr ? &ps->head->states[0] : nullptr;
}

StateInfo *position_storage_push(PositionStorage *ps) {
    if (ps->tail == nullptr)
        return nullptr;

    if (ps->used == POSITION_STORAGE_CHUNK) {
        // Reuse a chunk already appended by an earlier, longer line before allocating:
        // the arena only ever grows, so a reset leaves the tail chain in place.
        PositionStorageChunk *next = ps->tail->next;
        if (next == nullptr) {
            next = chunk_new();
            if (next == nullptr)
                return nullptr;
            ps->tail->next = next;
        }
        ps->tail = next;
        ps->used = 0;
    }

    StateInfo *st = &ps->tail->states[ps->used];
    memset(st, 0, sizeof *st);
    ps->used += 1;
    ps->count += 1;
    return st;
}
