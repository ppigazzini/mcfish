// Own the shared transposition table: the entry record the table is built from,
// the cluster probe, the replacement policy and the generation entries age by.
//
// The table is LOSSY by design, and that is the invariant every caller depends
// on: a probe may miss a position that was stored, a store may be refused, and a
// store may evict a deeper entry. Nothing may depend on a probe hitting — the
// search must stay correct with the table cleared or absent.
//
// `depth8 == 0` is the cheap occupancy test, so a stored depth is biased by
// DEPTH_ENTRY_OFFSET and the empty entry reads back as that depth (tt.cpp:51).
// `gen_bound8` packs three fields into one byte: the is-PV flag in bit 7, the
// Bound in bits 6..5 and the generation in bits 4..0. The generation wraps within
// those five bits, so an entry's relative age is a borrowing unsigned subtract —
// signed arithmetic there is undefined on overflow and would not borrow.
//
// Upstream: src/tt.h, src/tt.cpp. Port source: zfish src/engine/search/tt.zig,
// src/engine/state/tt_types.zig.

#ifndef CCFISH_TT_H
#define CCFISH_TT_H

#include "../board/score.h"
#include "../board/types.h"

#include <stddef.h>
#include <stdint.h>

typedef enum : uint8_t {
    BOUND_NONE = 0,
    BOUND_UPPER = 1,  // value is an upper bound (fail-low)
    BOUND_LOWER = 2,  // value is a lower bound (fail-high)
    BOUND_EXACT = 3,
} Bound;

// Bias every stored depth by this, so `depth8 == 0` means "unoccupied" rather
// than "searched to depth 0" (tt.cpp:51). Upstream spells the constant DEPTH_NONE
// (types.h:241); search_common.h owns that name alongside DEPTH_QS and
// DEPTH_UNSEARCHED and cannot be included here without a cycle, so carry the same
// value under the name the offset has upstream today.
enum : int32_t { DEPTH_ENTRY_OFFSET = -3 };

// Hold one position, in ten bytes (tt.cpp:62). The field order is the order
// tt_probe reads them in, because memory is fastest sequentially, and tt_save
// stores them in that same order.
typedef struct {
    uint16_t key16;
    uint8_t depth8;
    uint8_t gen_bound8;
    Move move16;
    int16_t value16;
    int16_t eval16;
} TTEntry;

// Carry a copy of the data an entry already held (tt.h:44). Reads are racy and
// non-atomic, so a copy may be self-inconsistent; it is still a copy, and it does
// not change under the caller once taken.
typedef struct {
    Move move;
    Value value;
    Value eval;
    int32_t depth;
    Bound bound;
    bool is_pv;
} TTData;

// Return the three things a probe yields (tt.cpp:254): whether the entry already
// held data on this position, the copy of that data, and the entry to write
// through — which on a miss is the least valuable entry of the cluster.
typedef struct {
    bool found;
    TTData data;
    TTEntry *writer;
} TTProbeResult;

// Size the table to MB megabytes and clear it. Return false when the allocation
// fails; the caller must then not probe.
bool tt_resize(size_t mb);
void tt_free(void);

// Zero every cluster and reset the generation to 0 (tt.cpp:191).
void tt_clear(void);

// Advance the generation, wrapping within its five bits so it never spills into
// the bound or is-PV bits (tt.cpp:240). Call once per root search.
void tt_new_search(void);

// Report the age entries are written with (tt.cpp:247).
uint8_t tt_generation(void);

// Look the position up. On a hit return the entry's data and a writer to it; on a
// miss return the empty data and a writer to the cluster entry worth least, where
// an entry's worth is its depth minus eight times its relative age (tt.cpp:254).
TTProbeResult tt_probe(Key key);

// Write through WRITER, overwriting a less valuable entry (tt.cpp:92). The write
// is non-atomic, may be racy, and may be declined outright.
void tt_save(TTEntry *writer, Key k, Value v, bool pv, Bound b, int32_t d, Move m, Value ev);

// Decrement a stored depth by PENALTY, saturating at 0 (tt.cpp:144).
void tt_penalize(TTEntry *writer, int32_t penalty);

// Report the fill in permille, as UCI `hashfull` wants it, counting only entries
// no older than MAX_AGE generations (tt.cpp:229).
int32_t tt_hashfull(int32_t max_age);

// Adjust a stored mate score for the current distance from the root. A mate score
// is stored relative to the node it was found at, so it must be re-based on both
// store and probe or the reported mate distance drifts.
static inline Value value_to_tt(Value v, int ply) {
    return v >= VALUE_MATE_IN_MAX_PLY  ? (Value) (v + ply)
         : v <= VALUE_MATED_IN_MAX_PLY ? (Value) (v - ply)
                                       : v;
}

static inline Value value_from_tt(Value v, int ply) {
    return v == VALUE_NONE             ? VALUE_NONE
         : v >= VALUE_MATE_IN_MAX_PLY  ? (Value) (v - ply)
         : v <= VALUE_MATED_IN_MAX_PLY ? (Value) (v + ply)
                                       : v;
}

#endif  // CCFISH_TT_H
