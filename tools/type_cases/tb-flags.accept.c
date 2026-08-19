// The legal form: the ranking takes the flag that belongs to it, by name.
#include "engine/search/root_move_build.h"
void right(Position *pos, StateInfo *st, RankedRootMove *r, size_t n);
void right(Position *pos, StateInfo *st, RankedRootMove *r, size_t n) {
    (void) tb_rank_moves(pos, st, r, n, RANK_DTZ_YES, 0, false, nullptr, nullptr);
    (void) tb_rank_moves(pos, st, r, n, RANK_DTZ_NO, 0, false, nullptr, nullptr);
}
