// Own the StateInfo arena: the chain of per-ply records that backs a position and
// the moves applied to it from the UCI `position ... moves ...` line.
//
// POINTER STABILITY IS THE WHOLE CONTRACT. `pos_do_move` writes into the newest
// record while every earlier one stays referenced — by `st->previous`, by the
// repetition walk, and by the search's own frames. A record's address must
// therefore never change once handed out. That rules out the obvious
// implementation (one growable array of StateInfo), because a reallocation would
// move records that are still live and turn the whole chain into dangling
// pointers. Each record here is its own allocation, which is stable by
// construction.
//
// The list starts non-empty: one zeroed root. `state_list_reset` returns it to
// exactly that. A root with `plies_from_null == 0` is what bounds every backward
// walk in `repetition.c`, so a root that is not zeroed is a read off the end of
// the chain.
//
// Every allocating call returns NULL on failure and leaves the list unchanged and
// usable — the list is legitimately growable (bounded by the move list a client
// sends, not by MAX_PLY), so out-of-memory is an error to propagate, not to
// assert on.
//
// Ported from zfish `engine/board/state_list.zig` (StateList,
// PendingStateStorage). Golden: `Stockfish/src/uci.h` (`StateListPtr`) and
// `Stockfish/src/uci.cpp: UCIEngine::position`.

#ifndef MCFISH_STATE_LIST_H
#define MCFISH_STATE_LIST_H

#include <stddef.h>

#include "position_types.h"
#include "types.h"

typedef struct StateList StateList;

// Create a list holding a single zeroed root StateInfo. Return NULL on failure.
StateList *state_list_create(void);

// Free the list and every StateInfo in it. Accept NULL.
void state_list_destroy(StateList *list);

// Drop to a single fresh zeroed root and return its address. Cannot fail: the root
// block already exists, so this only frees and zeroes.
StateInfo *state_list_reset(StateList *list);

// Append one zeroed StateInfo and return its address. Return NULL on failure,
// leaving the list unchanged.
StateInfo *state_list_push(StateList *list);

// Return the address of the most recently appended StateInfo. Undefined on an
// empty list, which only a moved-out list can be.
StateInfo *state_list_back(const StateList *list);

bool state_list_has_states(const StateList *list);
size_t state_list_len(const StateList *list);

// Hold a StateList with MOVE semantics: `pending_states_move_out` hands ownership
// to the search and NULLs the wrapper, so a later destroy frees nothing. This is
// the handoff the `position` command relies on — the chain is built before the
// search starts and must outlive the builder.
typedef struct PendingStateStorage PendingStateStorage;

PendingStateStorage *pending_states_create(void);

// Free the wrapper, and the list it still owns if it was never moved out. Accept NULL.
void pending_states_destroy(PendingStateStorage *storage);

// Drop to a single fresh root and return its address. Re-create the list if it was
// moved out. Return NULL on failure.
StateInfo *pending_states_reset(PendingStateStorage *storage);

// Append one StateInfo. Return NULL on failure, or if the list was moved out.
StateInfo *pending_states_push(PendingStateStorage *storage);

bool pending_states_has_states(const PendingStateStorage *storage);

// Transfer the list to the caller and null the wrapper. Return NULL if it was
// already moved out. The caller becomes responsible for `state_list_destroy`.
StateList *pending_states_move_out(PendingStateStorage *storage);

#endif  // MCFISH_STATE_LIST_H
