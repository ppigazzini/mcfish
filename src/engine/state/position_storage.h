// Own the per-worker Position object and the StateInfo arena its state chain lives in.
//
// The board ALGORITHMS live in board/position.c; what this owns is the storage. The
// invariant that shapes it: a StateInfo chain is linked by `previous` pointers and
// `pos->st` points into it, so once a state is handed out its address must never move.
// That rules out a reallocating vector -- the arena is a list of fixed chunks, and
// growing appends a chunk instead of copying, which is why upstream uses a std::deque
// here and not a std::vector.
//
// The arena holds the SETUP states: one anchor plus one per move replayed by the UCI
// `position ... moves ...` command. The per-ply states the search pushes inside the
// recursion are the search's own, on its stack, and never come from here.
//
// Blocks come from the engine's page allocator, so this zone never names an OS
// allocator and every chunk arrives zeroed.
//
// Upstream: uci.cpp (StateListPtr states), position.h (Position, StateInfo). Port source:
// zfish src/engine/state/position_storage.zig, src/engine/state/page_alloc.zig.

#ifndef CCFISH_POSITION_STORAGE_H
#define CCFISH_POSITION_STORAGE_H

#include "../board/position.h"

#include <stdbool.h>
#include <stddef.h>

// Hold one arena chunk. Sized so a chunk is a few pages and a normal game never needs
// more than one, while a long analysis line simply appends another.
enum { POSITION_STORAGE_CHUNK = 64 };

typedef struct PositionStorageChunk {
    StateInfo states[POSITION_STORAGE_CHUNK];
    struct PositionStorageChunk *next;
} PositionStorageChunk;

typedef struct {
    Position pos;

    PositionStorageChunk *head;  // the first chunk; states[0] of it is the anchor
    PositionStorageChunk *tail;  // the chunk the next allocation comes from
    size_t used;                 // states handed out from `tail`
    size_t count;                // states handed out overall, at least 1 once initialised
} PositionStorage;

// Allocate the first chunk and hand out the anchor state, leaving the Position zeroed --
// the same shape as a value-initialised `pos` before pos_set runs. Return false on
// allocation failure, leaving PS empty and safe to free.
bool position_storage_init(PositionStorage *ps);

// Release every chunk and zero PS. Idempotent, and safe on a zeroed storage.
void position_storage_free(PositionStorage *ps);

// Drop back to the anchor state, keeping the chunks. Zero the anchor and the Position, so
// a following pos_set starts from the same state a fresh storage would.
void position_storage_reset(PositionStorage *ps);

// Return the anchor state -- the one pos_set is given.
StateInfo *position_storage_root(PositionStorage *ps);

// Hand out the next state, appending a chunk when the current one is full. The returned
// address is stable for the life of the storage. Return null on allocation failure.
StateInfo *position_storage_push(PositionStorage *ps);

static inline Position *position_storage_pos(PositionStorage *ps) { return &ps->pos; }

static inline size_t position_storage_count(const PositionStorage *ps) { return ps->count; }

#endif  // CCFISH_POSITION_STORAGE_H
