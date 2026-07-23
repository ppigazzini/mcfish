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
// `tt_move` must already be known pseudo-legal for `pos` — the picker returns it
// unchecked. Pass MOVE_NONE when there is no usable TT move.
//
// The move buffer is reused across stages: quiets are scored on top of the
// captures already consumed, so `cur` only ever moves forward and MAX_MOVES is
// enough for both lists.
//
// Golden: the upstream `movepick.cpp`. `see_ge` mirrors upstream
// `position.cpp: Position::see_ge` and lives in the board zone
// (`board/legality.h`), which movepick.c includes.

#ifndef MCFISH_MOVEPICK_H
#define MCFISH_MOVEPICK_H

#include "history.h"
#include "search_types.h"

#include "../board/movegen.h"
#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>

// Order the stages exactly as upstream does: `initMainStage` computes a stage by
// adding 1 when there is no TT move, so each *_TT stage must be immediately
// followed by its init stage.
enum {
    MP_MAIN_TT = 0,
    MP_CAPTURE_INIT,
    MP_GOOD_CAPTURE,
    MP_QUIET_INIT,
    MP_GOOD_QUIET,
    MP_BAD_CAPTURE,
    MP_BAD_QUIET,

    MP_EVASION_TT = 7,
    MP_EVASION_INIT,
    MP_EVASION,

    MP_PROBCUT_TT = 10,
    MP_PROBCUT_INIT,
    MP_PROBCUT,

    MP_QSEARCH_TT = 13,
    MP_QCAPTURE_INIT,
    MP_QCAPTURE,
};

typedef struct {
    const Position *pos;
    Histories *hist;
    Key pawn_key;
    // The frame the picker gathers continuation pages from, lazily, at the one
    // stage that scores them: QUIET_INIT fills all six slots, EVASION_INIT only
    // slot 0. Null for the ProbCut picker, whose stages read no page.
    const Stack *ss;
    // Continuation-history pages (ss-1) .. (ss-6). Quiet scoring reads slots
    // 0, 1, 2, 3 and 5; evasion scoring reads slot 0. Every slot a stage reads
    // is filled by that stage's init before the read.
    const SharedStat *cont_hist[6];
    int ply;

    Move tt_move;
    int stage;
    int threshold;  // probcut SEE threshold
    int depth;
    bool skip_quiets;

    size_t cur;
    size_t end_cur;
    size_t end_bad_captures;
    size_t end_captures;
    size_t end_generated;
    ExtMove moves[MAX_MOVES];
} MovePicker;

// Set up the main-search / qsearch picker. DEPTH <= 0 selects the qsearch stage
// chain; a non-empty checkers set selects the evasion chain regardless of depth.
// SS is the frame whose (ss-1)..(ss-6) continuation pages quiet/evasion scoring
// reads; the picker gathers them only when such a stage runs.
void movepick_init(MovePicker *mp,
                   const Position *pos,
                   Histories *h,
                   Key pawn_key,
                   Move tt_move,
                   int depth,
                   int ply,
                   const Stack *ss);

// Set up the ProbCut picker: captures only, kept when they pass SEE >= THRESHOLD.
void movepick_init_probcut(
  MovePicker *mp, const Position *pos, Histories *h, Move tt_move, int threshold);

// Return the next move, or MOVE_NONE when the picker is exhausted. Moves are
// PSEUDO-legal; the caller must still filter with pos_legal.
Move movepick_next(MovePicker *mp);

// Stop producing quiets from the next call on. Bad captures still follow.
static inline void movepick_skip_quiets(MovePicker *mp) { mp->skip_quiets = true; }

#endif  // MCFISH_MOVEPICK_H
