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
// Upstream: src/tt.h, src/tt.cpp.

#ifndef MCFISH_TT_H
#define MCFISH_TT_H

#include <stdatomic.h>
#include "../board/score.h"
#include "../board/types.h"

#include <stdatomic.h>

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
// Every field is a RELAXED atomic, as upstream declares all six
// (tt.cpp:82-87). The table is shared by every worker with no lock, and upstream
// says plainly that an entry write "is non-atomic and can be racy" -- what it
// relies on is that each FIELD is read and written indivisibly, so a torn or
// rematerialised half-value never reaches the search. Plain members are a data
// race in the C sense, which is undefined behaviour rather than a benign one:
// measured at 10185 TSan reports on an 8-thread depth-14 search before this.
//
// Relaxed, not seq_cst. There is no ordering to establish between fields -- the
// entry is validated by key16 after the read -- and on x86 a relaxed load or
// store is a plain move, which is what keeps Threads 1 executing the same
// instructions and the anchor exact.
typedef struct {
    _Atomic uint16_t key16;
    _Atomic uint8_t depth8;
    _Atomic uint8_t gen_bound8;
    _Atomic Move move16;
    _Atomic int16_t value16;
    _Atomic int16_t eval16;
} TTEntry;

// Read and write one entry field with relaxed ordering. Spelled as macros so the
// field name stays visible at the use site rather than becoming a pointer
// expression.
//
// USE THESE, never a bare `entry->depth8`. A plain access on an `_Atomic` member
// is SEQ_CST in C, which is not what upstream's RelaxedAtomic is: seq_cst stores
// are lock-prefixed on x86 and upstream's are plain moves. The compiler accepts
// the bare form silently, so nothing but this rule keeps the ordering upstream's.
#define TT_LOAD(field) atomic_load_explicit(&(field), memory_order_relaxed)
#define TT_STORE(field, v) atomic_store_explicit(&(field), (v), memory_order_relaxed)

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

enum { TT_CLUSTER_SIZE = 3 };

// Group the entries a key maps onto. A cluster is exactly 32 bytes: the entries
// plus two bytes of explicit padding, so a cluster never straddles a cache line
// in a 64-byte-aligned table (tt.cpp:156).
typedef struct {
    TTEntry entry[TT_CLUSTER_SIZE];
    uint8_t padding[2];
} TTCluster;

static_assert(sizeof(TTEntry) == 10, "TTEntry must stay a packed 10-byte record");
static_assert(sizeof(TTCluster) == 32, "TTCluster must fill exactly half a cache line");

// Hold the table handle: the cluster count, the cluster base, and the running
// generation the store path ages entries by (tt.h:79).
typedef struct {
    size_t cluster_count;
    TTCluster *table;
    uint8_t generation8;
} TranspositionTable;

// Hold the one table the engine and all its threads share (tt.h:79). mcfish keeps
// it as a file-scope singleton — the tt functions take no handle — rather than the
// per-Engine object upstream passes down; every worker shares this one instance.
// The instance lives in tt.c; it is visible here so tt_probe below can inline into
// the node bodies, the way LTO inlines upstream's TranspositionTable::probe into
// search<NodeType> — the probe runs once per node and the call boundary (call,
// 32-byte sret return, re-copy) is real per-node work upstream does not pay.
//
// `table` is never null: before the first tt_resize (and after tt_free) it points
// at the one-cluster fallback below, so no hot path tests for a missing table.
extern TranspositionTable TT;
extern TTCluster TTFallback[1];

// Size the table to MB megabytes and clear it. Return false when the allocation
// fails; the caller must then not probe.
bool tt_resize(size_t mb);
void tt_free(void);

// Dispatch the clear's span jobs onto the pool's threads. Installed by the pool owner
// (search_threads) whenever a worker set exists; every hook null reads as a one-thread
// pool, so tt_clear zeroes the whole table on the calling thread — the correct serial
// clear for the headless/no-pool build, and the path the one-thread anchor takes.
typedef struct {
    void *ctx;
    size_t (*thread_count)(void *ctx);
    void (*run_on_thread)(void *ctx, size_t index, void (*job)(void *job_ctx), void *job_ctx);
    void (*wait_all)(void *ctx);
} TTClearThreads;

extern TTClearThreads TTClearPool;

// Zero every cluster and reset the generation to 0 (tt.cpp:191). With a pool installed
// above, dispatch one disjoint span per pool thread, as upstream's clear does
// (tt.cpp:184): the spans partition the cluster range exactly, so the parallel writes
// together equal the single memset and need no ordering between them.
void tt_clear(void);

// Advance the generation, wrapping within its five bits so it never spills into
// the bound or is-PV bits (tt.cpp:240). Call once per root search.
void tt_new_search(void);

// Report the age entries are written with (tt.cpp:247).
uint8_t tt_generation(void);

// Pack pv, bound and generation into gen_bound8 (tt.cpp:55). The generation takes
// the low bits so a wrapping increment never disturbs the two above it.
enum : uint8_t {
    TT_GENERATION_BITS = 5,
    TT_GENERATION_MASK = (1u << TT_GENERATION_BITS) - 1u,
    TT_BOUND_SHIFT = TT_GENERATION_BITS,
    TT_BOUND_MASK = 3u << TT_BOUND_SHIFT,
    TT_PV_SHIFT = TT_BOUND_SHIFT + 2,
    TT_PV_MASK = 1u << TT_PV_SHIFT,
};

// Take the high 64 bits of the 128-bit product, so a key maps onto the cluster
// range without a modulo (misc.h mul_hi64, used by tt.cpp:278).
static inline uint64_t tt_mul_hi64(uint64_t a, uint64_t b) {
    return (uint64_t) (((__uint128_t) a * (__uint128_t) b) >> 64);
}

// Report this entry's age. Count generations the way clocks count hours: require
// 0 - 1 == 31. The subtraction is unsigned so it borrows regardless of the pv and
// bound bits sitting above the generation (tt.cpp:127).
static inline uint8_t tt_entry_relative_age(const TTEntry *entry, uint8_t curr_generation) {
    return (uint8_t) (((uint32_t) curr_generation - (uint32_t) TT_LOAD(entry->gen_bound8))
                      & TT_GENERATION_MASK);
}

static inline bool tt_entry_is_occupied(const TTEntry *entry) {
    return TT_LOAD(entry->depth8) != 0;
}

// Convert the internal bitfields to external types (tt.cpp:65). Load depth8 and
// gen_bound8 once each and derive both of their fields from the one copy: the
// members are relaxed atomics, so the compiler may not merge repeated loads
// itself, and the second load of each would be a real extra read per probe hit.
static inline TTData tt_entry_read(const TTEntry *entry, uint8_t depth8) {
    const uint8_t gen_bound8 = TT_LOAD(entry->gen_bound8);
    return (TTData) {
        .move = TT_LOAD(entry->move16),
        .value = (Value) TT_LOAD(entry->value16),
        .eval = (Value) TT_LOAD(entry->eval16),
        .depth = DEPTH_ENTRY_OFFSET + (int32_t) depth8,
        .bound = (Bound) ((gen_bound8 & TT_BOUND_MASK) >> TT_BOUND_SHIFT),
        .is_pv = (gen_bound8 & TT_PV_MASK) != 0,
    };
}

static inline TTData tt_empty_data(void) {
    return (TTData) {
        .move = MOVE_NONE,
        .value = VALUE_NONE,
        .eval = VALUE_NONE,
        .depth = DEPTH_ENTRY_OFFSET,
        .bound = BOUND_NONE,
        .is_pv = false,
    };
}

// Look the position up. On a hit return the entry's data and a writer to it; on a
// miss return the empty data and a writer to the cluster entry worth least, where
// an entry's worth is its depth minus eight times its relative age (tt.cpp:254).
__attribute__((always_inline)) static inline TTProbeResult tt_probe(Key key) {
    // Use the low 16 bits as the key inside the cluster; the high bits already
    // chose the cluster. TT.table always points at a real table — the one-cluster
    // fallback before any resize — so the probe carries no null test, exactly as
    // upstream's probe carries none (tt.cpp:254).
    const uint16_t key16 = (uint16_t) key;
    TTEntry *const tte = TT.table[tt_mul_hi64(key, TT.cluster_count)].entry;

    for (size_t i = 0; i < TT_CLUSTER_SIZE; ++i)
        if (TT_LOAD(tte[i].key16) == key16) {
            // This gap is the main place for read races. Once tt_entry_read
            // returns, the copy is final, though it may be self-inconsistent.
            // Read depth8 once; it answers both the occupancy test and the
            // entry's stored depth.
            const uint8_t depth8 = TT_LOAD(tte[i].depth8);
            return (TTProbeResult) { .found = depth8 != 0,
                                     .data = tt_entry_read(&tte[i], depth8),
                                     .writer = &tte[i] };
        }

    // Pick the entry to replace: the least valuable one, where value is depth
    // minus eight times relative age (tt.cpp:266).
    TTEntry *replace = tte;
    for (size_t i = 1; i < TT_CLUSTER_SIZE; ++i)
        if ((int32_t) TT_LOAD(replace->depth8)
              - 8 * (int32_t) tt_entry_relative_age(replace, TT.generation8)
            > (int32_t) TT_LOAD(tte[i].depth8)
                - 8 * (int32_t) tt_entry_relative_age(&tte[i], TT.generation8))
            replace = &tte[i];

    return (TTProbeResult) { .found = false, .data = tt_empty_data(), .writer = replace };
}

// Preload the cluster KEY maps onto into cache. A non-blocking hint issued a few
// instructions ahead of the matching tt_probe, so the line is arriving by the time
// the probe reads it (tt.cpp:150, called from the search do_move). A no-op before
// the table exists; the hint changes no value, only when the line lands.
void tt_prefetch(Key key);

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

#endif  // MCFISH_TT_H
