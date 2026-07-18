// Expose the Syzygy tablebase facade: the one surface the engine and the shell
// call. Everything under syzygy/ is an implementation detail of this header.
//
// `tablebase_init` scans a SyzygyPath and is the only way tables enter the
// process; it may be called again for a new path and releases the previous set.
// Until it is called with a non-empty path, every probe reports `available == 0`
// and `tablebase_max_cardinality` is 0 — which is the normal state of an engine
// with no tablebases installed, and the state `bench` runs in.
//
// Golden: upstream `syzygy/tbprobe.h`. Port source: zfish `platform/tablebase.zig`.

#ifndef CCFISH_TABLEBASE_H
#define CCFISH_TABLEBASE_H

#include "syzygy/probe.h"

#include "../engine/board/position.h"

#include <stdbool.h>
#include <stddef.h>

// (Re)scan PATH — a colon-separated directory list — for `.rtbw`/`.rtbz` files.
// An empty path clears the registry. Not thread-safe, as upstream is not: call it
// only when no search is running.
void tablebase_init(const char *path, size_t path_len);

// Report the largest piece count the prober can serve, and 0 when no path is set,
// so a search reads one value to decide whether to probe at all.
size_t tablebase_max_cardinality(void);

// Report what the scan found on disk, for the startup message.
size_t tablebase_found_wdl(void);
size_t tablebase_found_dtz(void);

// Probe a position given as FEN text for both WDL and DTZ.
TbProbeResult tablebase_probe_fen(const char *fen, size_t len, bool chess960);

// Probe the WDL of the live search position, in search.
TbProbeResult tablebase_probe_wdl_pos(Position *pos);

#endif  // CCFISH_TABLEBASE_H
