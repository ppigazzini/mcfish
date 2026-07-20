// Own the score classification the UCI reporter prints: split a search Value into
// mate / tablebase / internal-units, and carry the plies-to-outcome each implies.
//
// Own the tablebase thresholds too. `types.h` stops at VALUE_MATE_IN_MAX_PLY, and
// the TB band sits immediately below it — VALUE_TB is one below the mate band, and
// the TB win/loss bands are MAX_PLY wide on each side. That layering is the
// invariant: |v| > VALUE_TB means mate, VALUE_TB_WIN_IN_MAX_PLY <= |v| <= VALUE_TB
// means a tablebase result, and anything nearer zero is a normal evaluation.
//
// Golden: `Stockfish/src/score.cpp:29`
// (Score::Score) and `Stockfish/src/types.h:158-178` (the thresholds and
// is_win/is_loss/is_decisive).

#ifndef MCFISH_SCORE_H
#define MCFISH_SCORE_H

#include "types.h"

// Place the tablebase band directly below the mate band (Stockfish types.h:161).
enum : int32_t {
    VALUE_TB = VALUE_MATE_IN_MAX_PLY - 1,
    VALUE_TB_WIN_IN_MAX_PLY = VALUE_TB - MAX_PLY,
    VALUE_TB_LOSS_IN_MAX_PLY = -VALUE_TB_WIN_IN_MAX_PLY,
};

// Name the three outcomes the classifier distinguishes, so a consumer switches on
// them exhaustively instead of on 0/1/2 with a default.
typedef enum : uint8_t {
    SCORE_NON_DECISIVE,
    SCORE_MATE,
    SCORE_TABLEBASE,
} ScoreKind;

// Report the classification. `plies` is the signed distance to the outcome — zero
// for SCORE_NON_DECISIVE — and `win` says which side the outcome favours.
typedef struct {
    ScoreKind kind;
    int32_t plies;
    bool win;
} ScoreClass;

// Classify VALUE against the three thresholds, which the caller supplies rather
// than reading off this header: the search emit path owns them, and passing them
// keeps the classifier a pure function of its arguments.
ScoreClass score_classify(int32_t value,
                          int32_t value_tb_win_in_max_ply,
                          int32_t value_tb,
                          int32_t value_mate);

static inline bool value_is_win(Value v) { return v >= VALUE_TB_WIN_IN_MAX_PLY; }
static inline bool value_is_loss(Value v) { return v <= VALUE_TB_LOSS_IN_MAX_PLY; }
static inline bool value_is_decisive(Value v) { return value_is_win(v) || value_is_loss(v); }

#endif  // MCFISH_SCORE_H
