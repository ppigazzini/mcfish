#include "score.h"

// Answer whether VALUE is decisive against the caller's win threshold. Kept
// private and threshold-parameterised; the header's value_is_decisive is the
// types.h-anchored public form.
static bool is_decisive_at(int32_t value, int32_t value_tb_win_in_max_ply) {
    return value >= value_tb_win_in_max_ply || value <= -value_tb_win_in_max_ply;
}

ScoreClass score_classify(int32_t value,
                          int32_t value_tb_win_in_max_ply,
                          int32_t value_tb,
                          int32_t value_mate) {
    if (!is_decisive_at(value, value_tb_win_in_max_ply))
        return (ScoreClass) { .kind = SCORE_NON_DECISIVE, .plies = 0, .win = false };

    const int32_t abs_value = value < 0 ? -value : value;

    if (abs_value <= value_tb) {
        const int32_t distance = value_tb - abs_value;
        return (ScoreClass) { .kind = SCORE_TABLEBASE,
                              .plies = value > 0 ? distance : -distance,
                              .win = value > 0 };
    }

    const int32_t distance = value_mate - abs_value;
    return (ScoreClass) { .kind = SCORE_MATE,
                          .plies = value > 0 ? distance : -distance,
                          .win = value > 0 };
}
