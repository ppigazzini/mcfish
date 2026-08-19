// "39 functions here carry [[nodiscard]]. It was a warning, so discarding a
// `registry_map_wdl` or `pos_set` result compiled -- and two did, in the test suite,
// where a rejected FEN would have left the next assertion reading a stale position."
// REQUIRES: -Werror=unused-result
#include "engine/board/position.h"
void wrong(Position *pos, const char *fen, StateInfo *si);
void wrong(Position *pos, const char *fen, StateInfo *si) { pos_set(pos, fen, false, si); }
