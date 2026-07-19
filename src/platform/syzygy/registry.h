// Own the Syzygy tables themselves: the material-key to TBTable map built by
// scanning SyzygyPath, the lazy mmap of a table's `.rtbw`/`.rtbz` file, and the
// parse of that file's per-(side, file) PairsData records.
//
// Keep the probe ALGORITHM out of this file — wdl.c and probe.c import this one
// and never the reverse, so neither side becomes a god-file. A TBTable's mapped
// pointers stay valid until the next `registry_init`, which unmaps every file and
// frees every allocation at once.
//
// `registry_init` is NOT thread-safe and is not called concurrently: upstream
// runs it from the `SyzygyPath` option callback, off the search. The two lazy
// maps ARE, because every probing thread reaches them — upstream says so at
// tbprobe.cpp:1266 ("Function is thread safe and can be called concurrently").
//
// A material key is computed here rather than read from Position, which carries
// none: only self-consistency matters, because the key never leaves this module.
//
// Mirror upstream `syzygy/tbprobe.cpp:1397` (Tablebases::init), `:1206` (set),
// `:1158` (set_dtz_map).

#ifndef MCFISH_SYZYGY_REGISTRY_H
#define MCFISH_SYZYGY_REGISTRY_H

#include "../thread_runtime.h"
#include "tables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TBTable {
    uint64_t key;
    uint64_t key2;
    int32_t piece_count;
    bool has_pawns;
    bool has_unique_pieces;
    uint8_t pawn_count[2];
    size_t sides;  // WDL: 2 when key != key2, else 1. DTZ is always one-sided.
    char stem[8];  // canonical file stem, e.g. "KQvK"
    size_t stem_len;

    // WDL (.rtbw): two sides by up to four files.
    //
    // `ready` is the publication flag for everything below it, and it is read
    // without the lock on the fast path, so it must be atomic: see
    // registry_map_wdl. Golden: tbprobe.cpp:384 (`std::atomic_bool ready`).
    AtomicBool ready;
    const uint8_t *base;  // whole mapped file, nullptr when the load failed
    size_t base_size;
    PairsData items[2][4];

    // DTZ (.rtbz): one side by up to four files, plus the value-remap table.
    AtomicBool dtz_ready;
    const uint8_t *dtz_base;
    size_t dtz_base_size;
    const uint8_t *dtz_map;  // set_dtz_map: base of the DTZ value maps
    size_t dtz_map_size;
    PairsData dtz_items[1][4];
} TBTable;

// (Re)build the registry for PATH — a colon-separated directory list — by
// enumerating every material configuration up to 7 men and registering the ones
// whose `.rtbw` file exists. Unmap and free everything the previous call owned.
// An empty path leaves the registry empty and every probe unavailable.
void registry_init(const char *path, size_t path_len);

// Report whether a SyzygyPath has been set, so a probe can leave early.
bool registry_ready(void);

size_t registry_max_cardinality(void);
size_t registry_discovered_max(void);
size_t registry_found_wdl(void);
size_t registry_found_dtz(void);

// Find the table serving KEY, or nullptr.
TBTable *registry_get(uint64_t key);

// Select one PairsData: WDL uses items[stm % sides][f], DTZ is one-sided.
// Mirror upstream `entry->get(stm, f)`.
PairsData *tbtable_get(TBTable *t, bool dtz, size_t stm, size_t f);

// Map and parse T's file on first use. Return false — permanently, for this
// registry generation — when the file is missing, truncated or corrupt.
bool registry_map_wdl(TBTable *t);
bool registry_map_dtz(TBTable *t);

// Compute the material key of a piece-count array indexed by Piece (16 entries,
// `color << 3 | type`). Registry keys and probed positions must both come from
// here or a lookup silently misses.
uint64_t syzygy_material_key(const int counts[16]);

#endif  // MCFISH_SYZYGY_REGISTRY_H
