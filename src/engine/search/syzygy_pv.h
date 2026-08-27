// Correct and extend the principal variation of a root move that scored from a
// tablebase.
//
// A search PV under a TB score is only trustworthy as far as it stays on
// top-ranked moves: past that point it is whatever the search happened to store,
// and printing it claims a win line the tablebase does not endorse. This truncates
// the PV to the verified prefix and then extends it with minimum-DTZ moves, so the
// line shown ends in mate rather than in the middle of nowhere.
//
// The invariant the caller depends on: POS is restored exactly, on every exit
// path, by undoing precisely the moves this made. The PV length and the number of
// moves made are the same counter — let them drift and the undo walk either leaves
// a move on the board or unmakes one that was never made.
//
// Golden: `Stockfish/src/search.cpp:2098` (syzygy_extend_pv).

#ifndef MCFISH_SYZYGY_PV_H
#define MCFISH_SYZYGY_PV_H

#include "search_types.h"

#include "../board/position.h"

// Truncate RM's PV to its tablebase-verified prefix and extend it toward mate.
// Set *V to VALUE_DRAW when the extended line ends in a draw — the exceptional
// case where a rounded DTZ counter mis-ranked a position reached with a
// non-optimal fifty-move counter.
//
// USE_DEADLINE gates the deadline: with no clock to respect there is no budget to
// exceed, and the extension always runs to completion. Its two halves belong to
// the caller -- a managed clock AND not `nodestime`, under which the do_move calls
// this makes never reach the node counter the clock is (upstream 19a02f44).
//
// MULTIPV divides the budget. Every reported line is extended, so N lines each
// granted half of Move Overhead would spend N/2 of it; the divisor keeps the whole
// report inside the half a single line had.
void syzygy_extend_pv(Position *pos, bool use_deadline, size_t multipv, RootMove *rm, int32_t *v);

#endif  // MCFISH_SYZYGY_PV_H
