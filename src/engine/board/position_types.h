// Own the plain-data board types — `StateInfo` and `Position` — as the leaf every
// other board module includes.
//
// The split is a cycle break, not a tidy-up. `fen_parse`, `legality`,
// `position_query`, `position_snapshot` and `state_list` all need the two structs
// and nothing else; routing them through the full `position.h` would drag the
// make/unmake surface into every leaf and make `state_list` depend on the mover it
// is supposed to be independent of.
//
// INVARIANT while the split is in progress: `position.h` still DEFINES both
// structs, so this header forwards to it rather than defining them a second time —
// two definitions of `struct StateInfo` in one translation unit is a hard error,
// and `position.h` is owned by another change in flight. When `position.h` is
// split, move the two `typedef struct { ... }` blocks verbatim into this header
// and have `position.h` include it. No include site changes when that happens.
//
// Ported from zfish `engine/board/position_types.zig`. Golden:
// `Stockfish/src/position.h` (struct StateInfo, class Position).

#ifndef CCFISH_POSITION_TYPES_H
#define CCFISH_POSITION_TYPES_H

#include "position.h"
#include "types.h"

#endif  // CCFISH_POSITION_TYPES_H
