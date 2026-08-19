// THE DOCUMENTED HOLE, PINNED FROM THE OTHER SIDE. docs/09-type-design.md's "What a
// compile error does NOT stop" says: "`Key`, `Value`, `Bitboard`, `Move` and
// `TimePoint` are aliases, not types. A `Key` where a `Bitboard` belongs compiles
// silently." If this file ever stops compiling, that section has gone stale and the
// page is claiming less than the tree now does -- which is a docs finding, not a win.
#include "engine/board/types.h"
static Bitboard take_bitboard(Bitboard b) { return b; }
Bitboard hole(Key k);
Bitboard hole(Key k) { return take_bitboard(k); }
