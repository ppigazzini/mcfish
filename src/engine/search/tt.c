#include "tt.h"

#include "../state/tt_types.h"

#include <stdlib.h>
#include <string.h>

// Pack pv, bound and generation into gen_bound8 (tt.cpp:55). The generation takes
// the low bits so a wrapping increment never disturbs the two above it.
enum : uint8_t {
    GENERATION_BITS = 5,
    GENERATION_MASK = (1u << GENERATION_BITS) - 1u,
    BOUND_SHIFT = GENERATION_BITS,
    BOUND_MASK = 3u << BOUND_SHIFT,
    PV_SHIFT = BOUND_SHIFT + 2,
    PV_MASK = 1u << PV_SHIFT,
};

// Hold the one table the engine and all its threads share (tt.h:79). mcfish still
// runs a single worker, so the handle is a file-scope singleton rather than an
// object the Engine graph passes down; the fields are the ones tt_types.h types.
static TranspositionTable TT = { .cluster_count = 0, .table = nullptr, .generation8 = 0 };

// Take the high 64 bits of the 128-bit product, so a key maps onto the cluster
// range without a modulo (misc.h mul_hi64, used by tt.cpp:278).
static uint64_t mul_hi64(uint64_t a, uint64_t b) {
    return (uint64_t) (((__uint128_t) a * (__uint128_t) b) >> 64);
}

// Subtract from a stored depth, saturating at 0 — upstream's `std::max(int(depth8)
// - n, 0)`, commented "guard against racy underflows, default to unoccupied"
// (tt.cpp:121, tt.cpp:146). A wrapping subtract would turn the shallowest entry
// into the deepest one, and would also make a cleared slot read as occupied.
static uint8_t depth_saturating_sub(uint8_t depth8, int32_t n) {
    const int32_t d = (int32_t) depth8 - n;
    return d > 0 ? (uint8_t) d : 0u;
}

// Report this entry's age. Count generations the way clocks count hours: require
// 0 - 1 == 31. The subtraction is unsigned so it borrows regardless of the pv and
// bound bits sitting above the generation (tt.cpp:127).
static uint8_t entry_relative_age(const TTEntry *entry, uint8_t curr_generation) {
    return (uint8_t) (((uint32_t) curr_generation - (uint32_t) TT_LOAD(entry->gen_bound8))
                      & GENERATION_MASK);
}

static bool entry_is_occupied(const TTEntry *entry) { return TT_LOAD(entry->depth8) != 0; }

// Convert the internal bitfields to external types (tt.cpp:65).
static TTData entry_read(const TTEntry *entry) {
    return (TTData) {
        .move = TT_LOAD(entry->move16),
        .value = (Value) TT_LOAD(entry->value16),
        .eval = (Value) TT_LOAD(entry->eval16),
        .depth = DEPTH_ENTRY_OFFSET + (int32_t) TT_LOAD(entry->depth8),
        .bound = (Bound) ((TT_LOAD(entry->gen_bound8) & BOUND_MASK) >> BOUND_SHIFT),
        .is_pv = (TT_LOAD(entry->gen_bound8) & PV_MASK) != 0,
    };
}

static TTData empty_data(void) {
    return (TTData) {
        .move = MOVE_NONE,
        .value = VALUE_NONE,
        .eval = VALUE_NONE,
        .depth = DEPTH_ENTRY_OFFSET,
        .bound = BOUND_NONE,
        .is_pv = false,
    };
}

bool tt_resize(size_t mb) {
    tt_free();

    const size_t cluster_count = mb * 1024 * 1024 / sizeof(TTCluster);
    if (cluster_count == 0)
        return false;

    // Round the request up to a whole number of cache lines: aligned_alloc wants a
    // size that is a multiple of the alignment, and a cluster is half a line.
    const size_t bytes = (cluster_count * sizeof(TTCluster) + 63) & ~(size_t) 63;
    TTCluster *const table = aligned_alloc(64, bytes);
    if (!table)
        return false;

    TT.table = table;
    TT.cluster_count = cluster_count;
    tt_clear();
    return true;
}

void tt_free(void) {
    free(TT.table);
    TT.table = nullptr;
    TT.cluster_count = 0;
    TT.generation8 = 0;
}

void tt_clear(void) {
    TT.generation8 = 0;
    if (TT.table)
        memset(TT.table, 0, TT.cluster_count * sizeof(TTCluster));
}

void tt_new_search(void) {
    // Don't overflow into the other bits of gen_bound8 (tt.cpp:240).
    TT.generation8 = (uint8_t) ((TT.generation8 + 1u) & GENERATION_MASK);
}

uint8_t tt_generation(void) { return TT.generation8; }

TTProbeResult tt_probe(Key key) {
    if (!TT.table)
        return (TTProbeResult) { .found = false, .data = empty_data(), .writer = nullptr };

    // Use the low 16 bits as the key inside the cluster; the high bits already
    // chose the cluster.
    const uint16_t key16 = (uint16_t) key;
    TTEntry *const tte = TT.table[mul_hi64(key, TT.cluster_count)].entry;

    for (size_t i = 0; i < TT_CLUSTER_SIZE; ++i)
        if (tte[i].key16 == key16)
            // This gap is the main place for read races. Once entry_read returns,
            // the copy is final, though it may be self-inconsistent.
            return (TTProbeResult) { .found = entry_is_occupied(&tte[i]),
                                     .data = entry_read(&tte[i]),
                                     .writer = &tte[i] };

    // Pick the entry to replace: the least valuable one, where value is depth
    // minus eight times relative age (tt.cpp:266).
    TTEntry *replace = tte;
    for (size_t i = 1; i < TT_CLUSTER_SIZE; ++i)
        if ((int32_t) TT_LOAD(replace->depth8)
              - 8 * (int32_t) entry_relative_age(replace, TT.generation8)
            > (int32_t) tte[i].depth8 - 8 * (int32_t) entry_relative_age(&tte[i], TT.generation8))
            replace = &tte[i];

    return (TTProbeResult) { .found = false, .data = empty_data(), .writer = replace };
}

void tt_save(TTEntry *writer, Key k, Value v, bool pv, Bound b, int32_t d, Move m, Value ev) {
    if (!writer)
        return;

    const uint16_t key16 = (uint16_t) k;

    // Preserve the old ttmove if we don't have a new one.
    if (m != MOVE_NONE || key16 != TT_LOAD(writer->key16))
        TT_STORE(writer->move16, m);

    // Overwrite less valuable entries, cheapest checks first (tt.cpp:100).
    if (b == BOUND_EXACT || key16 != TT_LOAD(writer->key16)
        || d - DEPTH_ENTRY_OFFSET + 2 * (int32_t) pv > (int32_t) TT_LOAD(writer->depth8) - 4
        || entry_relative_age(writer, TT.generation8) != 0) {
        TT_STORE(writer->key16, key16);
        TT_STORE(writer->depth8, (uint8_t) (d - DEPTH_ENTRY_OFFSET));
        TT_STORE(writer->gen_bound8, (uint8_t) (TT.generation8 | ((uint8_t) b << BOUND_SHIFT)
                                                | ((uint8_t) pv << PV_SHIFT)));
        TT_STORE(writer->value16, (int16_t) v);
        TT_STORE(writer->eval16, (int16_t) ev);
    }
    // Secondary aging. Important for elementary mate finding. Age a deep, decisive,
    // non-exact entry that this store is NOT overwriting (tt.cpp:113).
    else if ((int32_t) TT_LOAD(writer->depth8) + DEPTH_ENTRY_OFFSET >= 5
             && (Bound) ((TT_LOAD(writer->gen_bound8) & BOUND_MASK) >> BOUND_SHIFT)
                  != BOUND_EXACT) {
        // Keep upstream's inner test whole (tt.cpp:120): the `abs < VALUE_INFINITE`
        // half excludes an entry holding +/-VALUE_INFINITE, and is not the same
        // condition as the else-if above it.
        const Value v16 = (Value) TT_LOAD(writer->value16);
        const Value abs16 = v16 < 0 ? -v16 : v16;
        if (abs16 < VALUE_INFINITE && value_is_decisive(v16))
            TT_STORE(writer->depth8, depth_saturating_sub(writer->depth8, 1));
    }
}

void tt_penalize(TTEntry *writer, int32_t penalty) {
    if (!writer)
        return;
    // Guard against racy underflows, defaulting to "unoccupied" (tt.cpp:146).
    TT_STORE(writer->depth8, depth_saturating_sub(writer->depth8, penalty));
}

int32_t tt_hashfull(int32_t max_age) {
    if (!TT.table)
        return 0;

    // Sample the first thousand clusters rather than the whole table: the figure is
    // a UCI display value and a full scan is O(table) on every info line.
    int32_t cnt = 0;
    const size_t limit = TT.cluster_count < 1000 ? TT.cluster_count : 1000;

    for (size_t i = 0; i < limit; ++i)
        for (size_t j = 0; j < TT_CLUSTER_SIZE; ++j) {
            const TTEntry *const entry = &TT.table[i].entry[j];
            if (entry_is_occupied(entry)
                && (int32_t) entry_relative_age(entry, TT.generation8) <= max_age)
                ++cnt;
        }

    return cnt / TT_CLUSTER_SIZE;
}
