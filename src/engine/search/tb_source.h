// Inject the Syzygy tablebase probe.
//
// Probing is disk I/O — a platform service — so the search reaches it through
// function pointers the platform registers at startup. The probe result type
// lives here because it is a search-facing value; the platform prober re-exports
// it.
//
// Treat these three as GENUINELY SAFE unregistered: the default IS the right
// answer when the subsystem is absent. "No tablebases are loaded" is exactly true
// with no prober attached, and a search that does not probe is the correct
// search, not a degraded one.
//
// Ported from zfish `engine/search/tb_source.zig`.

#ifndef MCFISH_TB_SOURCE_H
#define MCFISH_TB_SOURCE_H

#include "../board/position.h"

#include <stddef.h>

// Report one probe. `available == 0` means FAIL / no result; every other field is
// unspecified in that case.
typedef struct {
    uint8_t available;
    int32_t wdl;
    int32_t wdl_state;
    int32_t dtz;
    int32_t dtz_state;
} TbProbeResult;

// Return the largest piece count the loaded tablebases cover; 0 when none are.
extern size_t (*TbMaxCardinality)(void);

// Probe a FEN.
extern TbProbeResult (*TbProbeFen)(const char *fen, size_t fen_len, bool chess960);

// Probe the WDL in-search on the live position (Step 6). Do/undo on POS for the
// capture recursion and restore it exactly.
extern TbProbeResult (*TbProbeWdlPos)(Position *pos);

#endif  // MCFISH_TB_SOURCE_H
