// The two flags the root ranking carries, transposed. Both inversions are silent and
// both are meaningful: Rule50 changes the verdict a table gives -- it is the option
// whose omission from the WDL path was defect 24 -- and RankDtz changes whether DTZ
// ranking happens at all. As two adjacent bools this compiled.
// REQUIRES: -Werror=enum-conversion
#include "engine/search/root_move_build.h"
void wrong(Position *pos, StateInfo *st, RankedRootMove *r, size_t n);
void wrong(Position *pos, StateInfo *st, RankedRootMove *r, size_t n) {
    RankDtz rank = RANK_DTZ_YES;
    (void) tb_rank_moves(pos, st, r, n, rank, 0, false, nullptr, nullptr);
    Rule50 fifty = RULE50_APPLY;
    (void) tb_rank_moves(pos, st, r, n, fifty, 0, false, nullptr, nullptr);
}
