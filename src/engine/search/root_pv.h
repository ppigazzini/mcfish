// Own the ROOT move's principal variation, which is a different type from a
// per-ply one because it has a different capacity contract.
//
// `PVMoves` (search_types.h) is a fixed `Move[MAX_PLY + 1]`, and that bound is
// exact: a search line cannot be longer than the recursion that produced it, and
// the recursion stops at MAX_PLY. A ROOT move's PV is not produced only by the
// search. `syzygy_extend_pv` walks on from where the search stopped, appending
// minimum-DTZ moves toward a tablebase mate, and that walk is bounded by the
// position rather than by the stack -- a six-man DTZ line runs past MAX_PLY plies
// (upstream search.h:64, search.cpp:2199).
//
// The capacity contract this type keeps:
//
//   * It is never below ROOT_PV_SEARCH_CAP, from construction onward. Every write
//     the SEARCH makes is inside that, so `root_pv_set_line` and `root_pv_copy`
//     reach the allocator on no line the search can produce, and neither reports
//     a status.
//   * Only `root_pv_push` -- the tablebase walk -- needs more, so it is the one
//     fallible operation. A refusal there ends the walk on the line already built.
//
// The buffer is OWNED. MOVING a RootMove by struct assignment is correct and is
// what the root list's sorts do -- an insertion sort and two rotations PERMUTE
// elements, so each buffer has exactly one owner before and after. DUPLICATING
// one is not: `previous_pv = pv` and the best-line save and restore go through
// `root_pv_copy`, and the per-worker seeding through `root_moves_copy`.

#ifndef MCFISH_ROOT_PV_H
#define MCFISH_ROOT_PV_H

#include "../board/types.h"

#include <stddef.h>

// The capacity every root PV is built with: the longest line the SEARCH can
// produce. One more than MAX_PLY because a root PV is the root move followed by
// a child PV of up to MAX_PLY moves.
enum { ROOT_PV_SEARCH_CAP = MAX_PLY + 1 };

typedef struct {
    Move *moves;
    size_t length;
    size_t capacity;
} RootPVMoves;

// Give PV a buffer of ROOT_PV_SEARCH_CAP moves and a length of zero. Return false
// only if the allocation fails, which leaves PV zeroed: every operation below
// accepts that shape, and the two that write retry the allocation.
[[nodiscard]] bool root_pv_init(RootPVMoves *pv);

// Release PV's buffer and zero it. A zeroed or already-released PV is a no-op.
void root_pv_release(RootPVMoves *pv);

// Set PV to FIRST followed by CHILD's LEN moves, upstream's `pv.resize(1)` then
// append (search.cpp:1495). CHILD may be null when LEN is zero.
void root_pv_set_line(RootPVMoves *pv, Move first, const Move *child, size_t len);

// Append M and report whether it fit. Grows past ROOT_PV_SEARCH_CAP when needed,
// so this is the one operation that allocates on a well-formed call -- and the
// one that can fail.
[[nodiscard]] bool root_pv_push(RootPVMoves *pv, Move m);

// Cut PV down to LEN moves. Shrinking only: raising a length this way would
// expose moves the caller never wrote.
void root_pv_truncate(RootPVMoves *pv, size_t len);

// Copy SRC's moves into DST, growing DST when the source outgrew its capacity.
// A growth that fails copies the prefix that fits: a report that lost its tail,
// where refusing outright would leave DST holding a line belonging to neither
// iteration.
void root_pv_copy(RootPVMoves *dst, const RootPVMoves *src);

#endif  // MCFISH_ROOT_PV_H
