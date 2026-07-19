// Own the Syzygy WDL probe: the position-to-index encoding (`do_probe_table`),
// the material-key lookup around it (`probe_table`), and the capture recursion
// upstream calls `search` — which is what a WDL probe actually is, because the
// stored value is wrong for a position whose every legal move zeroes the
// fifty-move counter.
//
// `search_wdl` does and undoes moves on the position it is given and restores it
// exactly, so the caller may hand it the live search position. It touches only
// the board and its StateInfo chain.
//
// Mirror upstream `syzygy/tbprobe.cpp:772` (do_probe_table), `:1305`
// (probe_table), `:1332` (search). Port source: zfish `platform/syzygy/wdl.zig`.

#ifndef MCFISH_SYZYGY_WDL_H
#define MCFISH_SYZYGY_WDL_H

#include "registry.h"

#include "../../engine/board/position.h"
#include "../../engine/board/types.h"

#include <stdbool.h>
#include <stdint.h>

// Define upstream's ProbeState.
enum : int32_t {
    PROBE_FAIL = 0,
    PROBE_OK = 1,
    PROBE_ZEROING = 2,
    PROBE_CHANGE_STM = -1,
};

// Define upstream's WDLScore, from the side to move's point of view.
enum : int32_t {
    WDL_LOSS = -2,
    WDL_BLESSED_LOSS = -1,
    WDL_DRAW = 0,
    WDL_CURSED_WIN = 1,
    WDL_WIN = 2,
};

typedef struct {
    int32_t value;
    int32_t state;
} TbProbeValue;

// Compute POS's material key with the registry's hash, so a lookup matches.
uint64_t syzygy_position_key(const Position *pos);

// Encode POS into T's index and decode the stored value. WDL yields the score in
// -2..2; DTZ yields the mapped distance, or sets *STATE to PROBE_CHANGE_STM when
// the table stores the other side. Set *STATE to PROBE_FAIL on a corrupt table.
int32_t
do_probe_table(const Position *pos, TBTable *t, bool dtz, int32_t wdl_score, int32_t *state);

// Look POS's material up, map its file on first use, and probe. Report a position
// no table serves by setting *STATE to PROBE_FAIL.
int32_t probe_table(const Position *pos, bool dtz, int32_t wdl_score, int32_t *state);

// Return the best of POS's own stored WDL and the WDL of its zeroing moves.
// CHECK_ZEROING folds in pawn moves as well as captures, which is what a DTZ
// probe needs; a WDL probe passes false.
TbProbeValue search_wdl(Position *pos, bool check_zeroing);

#endif  // MCFISH_SYZYGY_WDL_H
