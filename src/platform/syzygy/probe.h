// Own the DTZ probe and the two entry points the rest of the engine uses:
// `syzygy_probe_fen` for an arbitrary position given as text, and
// `syzygy_probe_wdl_pos` for the live search position.
//
// A DTZ probe is a WDL probe first: `probe_dtz` folds in the zeroing moves, then
// reads the DTZ table, and resolves the one-sided-table case with a one-ply
// search that minimises DTZ. `available == 0` is the single encoding for "no
// result" — no path set, no table for this material, or a file that would not
// map or parse. It is the normal answer with no tablebases installed.
//
// Mirror upstream `syzygy/tbprobe.cpp:1601` (probe_dtz), `:1569` (probe_wdl),
// `:177` (dtz_before_zeroing). Port source: zfish `platform/syzygy/wdl.zig`.

#ifndef CCFISH_SYZYGY_PROBE_H
#define CCFISH_SYZYGY_PROBE_H

#include "wdl.h"

#include "../../engine/board/position.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t available;  // 0 when no WDL result could be produced
    int32_t wdl;        // -2..2 from the side to move's point of view
    int32_t wdl_state;  // PROBE_OK or PROBE_ZEROING
    int32_t dtz;        // plies to zeroing, signed like wdl; 0 for a draw
    int32_t dtz_state;  // PROBE_FAIL when the DTZ half failed but WDL held
} TbProbeResult;

// Probe DTZ from the side to move's view. Set *STATE to PROBE_FAIL when any probe
// in the recursion failed. POS is restored exactly.
int32_t probe_dtz(Position *pos, int32_t *state);

// Probe FEN (LEN bytes, need not be NUL-terminated) for its WDL and DTZ.
TbProbeResult syzygy_probe_fen(const char *fen, size_t len, bool chess960);

// Probe the WDL of the live search position. Do and undo moves on POS for the
// capture recursion and restore it exactly; the accumulator stack is untouched.
TbProbeResult syzygy_probe_wdl_pos(Position *pos);

#endif  // CCFISH_SYZYGY_PROBE_H
