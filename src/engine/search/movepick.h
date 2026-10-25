// Own move ordering: the staged move picker and the static exchange evaluation
// it prunes captures with.
//
// The picker is a lazy state machine, not a sorted list. `movepick_next` walks
// the stages in order and generates only when a stage demands it, so a node that
// fails high on the TT move never runs the generator at all. The invariant every
// stage relies on: the TT move is returned FIRST and then skipped everywhere
// else, so the caller sees each move exactly once even though the TT move is
// also produced by the generator.
//
// `tt_move` must already be known legal for `pos` — the picker returns it
// unchecked. Pass MOVE_NONE when there is no usable TT move.
//
// The move buffer is reused across stages. Losing captures and losing quiets are
// copied, in the order they were met, into a prefix that grows behind the cursor,
// and quiets are scored over the good captures already consumed, so one buffer of
// MAX_MOVES holds both lists and the losers are replayed as one span at the end.
//
// Golden: the upstream `movepick.cpp`. `see_ge` mirrors upstream
// `position.cpp: Position::see_ge` and lives in the board zone
// (`board/legality.h`), which movepick.c includes.
//
// Upstream: movepick.h:36 (class MovePicker).

#ifndef MCFISH_MOVEPICK_H
#define MCFISH_MOVEPICK_H

#include "history.h"
#include "search_types.h"

#include "../board/movegen.h"
#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>

// Order the stages as upstream does (movepick.cpp:30). Every picker starts at MP_TT,
// or at MP_INIT_FIRST_STAGE when its TT move is not usable; what KIND of picker it is
// -- main search, quiescence, evasions or ProbCut -- is `first_stage`, the stage the
// init hands over to. The four stages from MP_GOOD_QUIET to MP_EVASION only walk a
// list, and are kept consecutive so movepick_next can reach them by one range test.
enum {
    MP_TT = 0,
    MP_INIT_FIRST_STAGE,

    MP_GOOD_CAPTURE,
    MP_GOOD_QUIET,
    MP_BAD_MOVES,

    MP_QSEARCH,

    MP_EVASION,

    MP_PROBCUT,
};

typedef struct {
    const Position *pos;
    Histories *hist;
    Key pawn_key;
    // The CALLER's continuation-page array, not a copy: `contHist` in upstream's
    // constructor (movepick.cpp:149). Slot k is the (ss-1-k) page. Quiet scoring
    // reads slots 0, 1, 2, 3 and 5, evasion scoring reads slot 0, and the
    // quiescence caller passes a one-entry array because its only scorer is the
    // evasion one (search.cpp:1732). Null for the ProbCut picker, whose stages
    // read no page. It must OUTLIVE the picker -- it is borrowed, not copied.
    const SharedStat *const *cont_hist;
    int ply;

    Move tt_move;
    int stage;
    int first_stage;
    int threshold;  // probcut SEE threshold
    int depth;
    bool skip_quiets;

    size_t cur;
    size_t end_cur;
    size_t end_bad_captures;
    size_t end_bad_moves;
    ExtMove moves[MAX_MOVES];
} MovePicker;

// Set up the main-search / qsearch picker. The caller must then select the
// opening stages (its mp_set_*_stage helper) before the first movepick_next:
// the choice reads the TT move's legality, which the search zone tests, so the
// picker does not compute a stage of its own.
// SS is the frame whose (ss-1)..(ss-6) continuation pages quiet/evasion scoring
// reads; the picker gathers them only when such a stage runs.
void movepick_init(MovePicker *mp,
                   const Position *pos,
                   Histories *h,
                   Key pawn_key,
                   Move tt_move,
                   int depth,
                   int ply,
                   const SharedStat *const *cont_hist);

// Set up the ProbCut picker: captures only, kept when they pass SEE >= THRESHOLD.
// The caller selects the opening stage afterwards, as with movepick_init.
void movepick_init_probcut(
  MovePicker *mp, const Position *pos, Histories *h, Move tt_move, int threshold);

// Return the next move, or MOVE_NONE when the picker is exhausted. Every move is
// legal: the generators emit nothing else.
Move movepick_next(MovePicker *mp);

// Stop producing quiets from the next call on. Bad captures still follow. A quiet
// stage already under way is cut short by moving the span ends back to the bad
// captures' end, which drops the quiets left in the current span and every losing
// quiet copied so far (upstream skip_quiet_moves, movepick.cpp:353).
static inline void movepick_skip_quiets(MovePicker *mp) {
    mp->skip_quiets = true;
    if (mp->stage == MP_BAD_MOVES || mp->stage == MP_GOOD_QUIET) {
        mp->end_cur = mp->end_bad_captures;
        mp->end_bad_moves = mp->end_bad_captures;
    }
}

#endif  // MCFISH_MOVEPICK_H
