// Own the search history tables and the writes that age, clear and update them.
//
// The tables are the whole move-ordering memory: butterfly (main), low-ply,
// capture, continuation, continuation-correction, and the two key-indexed shared
// tables (pawn, correction). They are one flat `Histories` block so a future
// per-worker port is a second instance, not a re-shape.
//
// The invariant the code cannot show: every entry is a gravity-bounded int16.
// `stats_update` never lets |entry| exceed the D it is called with, so the
// int16 narrowing at the end of it cannot lose a bit for any D used here
// (max 30000). Change a D above 32767 and the narrowing silently wraps.
//
// The stack-walking updates (update_all_stats / update_quiet_histories /
// update_continuation_histories) live in the search zone, as upstream keeps
// them in search.cpp; this module holds only the tables and the primitives
// that read and write single entries.
//
// Golden: the upstream `history.h` and `search.cpp` (update_all_stats /
// update_continuation_histories / update_quiet_histories /
// update_correction_history).

#ifndef MCFISH_HISTORY_H
#define MCFISH_HISTORY_H

#include "../board/position.h"
#include "../board/types.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HIST_UINT16 = 65536,                   // UINT_16_HISTORY_SIZE: one row per raw move word
    LOW_PLY_HISTORY_SIZE = 5,              // LOW_PLY_HISTORY_SIZE
    HIST_PIECE_TYPE_NB = 8,                // capture history's third index is 8-wide, not 7
    HIST_PIECETO = PIECE_NB * SQUARE_NB,   // one PieceToHistory page, indexed pc * 64 + to
    CORRECTION_HISTORY_BASE_SIZE = 65536,  // CORRHIST_BASE_SIZE, the size at ONE thread
    PAWN_HISTORY_BASE_SIZE = 8192,         // PAWN_HISTORY_BASE_SIZE, the size at ONE thread
    CORRECTION_HISTORY_LIMIT = 1024,

    // Hold the continuation block's page count: continuationHistory[in_check][capture].
    CONTINUATION_PAGES = 2 * 2 * HIST_PIECETO,
};

// State the cleared values of the two key-indexed tables once, here, because three places
// clear them: history_clear, the striped shared clear a worker runs for its own slice, and
// the bundle-level clear. NEITHER IS ZERO, so any of the three that reaches for memset is
// silently wrong -- the search reads the correction table on the first node after a clear
// and reads a different value than upstream from then on. Upstream: search.cpp:680-681.
enum : int16_t {
    CORRECTION_HISTORY_FILL = -5,
    PAWN_HISTORY_FILL = -1338,
};

// Hold one entry of a SHARED history table.
//
// Upstream's `StatsEntry<T, D, Shared>` stores a `RelaxedAtomic<T>` when Shared is set
// (history.h:62), and it sets it for exactly the three tables a NUMA node's workers
// share: the continuation block (`AtomicStats`), the pawn table (`DynStats<AtomicStats>`)
// and the correction bundle (`StatsEntry<i16, D, true>`). Every worker on the node writes
// them at once, so the entry is atomic -- and RELAXED, for the reason the node counters
// are: the value is a move-ordering heuristic, and no worker's correctness depends on
// observing another's write in any particular order.
//
// The per-worker tables -- main, low-ply, capture, continuation-correction -- stay plain
// `int16_t`, because upstream's `Stats` for those is the non-atomic instantiation.
typedef _Atomic int16_t SharedStat;

// The gravity limit a history table clamps its bonus to.
//
// A TYPE, not an `int`, and the distinction is the whole point. `stats_update`
// used to take the bonus and the clamp as two adjacent `int`s across eighteen
// call sites, and a transposition does not fail: it clamps by one and divides by
// the other, producing a differently shaped gravity curve and so a different move
// ordering. No gate but the bench signature can see that, and the signature says
// only that something moved.
//
// Upstream cannot express the swap at all: its limit is a TEMPLATE parameter, one
// per `StatsEntry<int16_t, D>` instantiation. C has no compile-time parameter, and
// both sibling ports closed this with the feature C lacks -- `comptime` in Zig, a
// const generic in Rust. What C23 does have is a distinct struct type plus
// `constexpr`, and together they are stronger than a named constant: a bare
// `constexpr int` would name the value and leave both parameters `int`, which
// stops nothing, while `HistLimit` converts to nothing at all, so BOTH directions
// of the transposition are a hard error.
//
// `constexpr` rather than an enum because the value is a struct, and rather than
// `static const` because it says the value is a constant expression. Both
// compilers this tree builds with implement it -- clang under `-std=c23` and gcc
// 13.3 under `-std=c2x`, which is what `detect_std_flag` selects there.
typedef struct {
    int v;
} HistLimit;

// One limit per table, mirroring upstream's per-instantiation `D`.
//
// Two tables that share a value keep SEPARATE constants deliberately. They are
// separate upstream tunables that happen to coincide today, and folding them into
// one name would make a sync that moves either one move both -- the drift hazard
// of a shared owner, which is the mirror image of the two-owners hazard.
static constexpr HistLimit HIST_LIMIT_MAIN = { 7183 };
static constexpr HistLimit HIST_LIMIT_LOW_PLY = { 7183 };
static constexpr HistLimit HIST_LIMIT_PAWN = { 8192 };
static constexpr HistLimit HIST_LIMIT_TT_MOVE = { 8192 };
static constexpr HistLimit HIST_LIMIT_CAPTURE = { 10692 };
static constexpr HistLimit HIST_LIMIT_CONTINUATION = { 30000 };

// Derived, not restated: `CORRECTION_HISTORY_LIMIT` stays an `int` enum because
// `search_common.c` reads it as `/ 4` and the tests assert against that quarter.
static constexpr HistLimit HIST_LIMIT_CORRECTION = { CORRECTION_HISTORY_LIMIT };

// Fold the widening into the load itself, where the compiler will not.
//
// clang lowers a two-byte relaxed atomic load to `movzwl <mem>,%r` and then a SEPARATE
// reg-reg `movswl`: LLVM's x86 ISel has no sextload pattern for ATOMIC_LOAD, so the
// node's result type is i16 and the widening cannot fold into it. A plain i16 load gets
// the one `movswl <mem>,%r`. Every C spelling of the atomic load produces the pair --
// `atomic_load_explicit`, `__atomic_load_n` through a plain pointer, and an explicit
// narrowing cast were each checked and each emits `movzwl` + `cwtl`.
//
// The cost is per LOAD, and these banks are the most-read memory in the engine: six
// planes per quiet move scored, two more per quiet move in the reduction's stat score,
// and four a node in the correction term.
//
// On x86-64 a naturally aligned two-byte load IS the relaxed atomic load, so the asm
// claims nothing the hardware does not already give; the three assertions hold what the
// operand assumes about the entry's layout and lock-freedom, so an implementation that
// padded or locked it would fail the build rather than miscompile the search. Inline asm
// carrying a memory operand is opaque to the compiler's memory analysis: it is not
// forwarded across a store to the same entry and not hoisted across a call, which makes
// it more ordered than the relaxed load it replaces, never less.
#if defined(__clang__) && defined(__x86_64__)
    #if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer) \
      && !__has_feature(memory_sanitizer)
        // The sanitizer lanes keep the atomic. Inline asm is invisible to every one of
        // them, and `./build.sh tsan` and `./build.sh test` exist to see exactly these
        // accesses -- so those lanes establish the access PATTERN and not the
        // instruction that ships, which is the limit of what they prove here.
        #define MCFISH_WIDE_ATOMIC_LOAD_ASM 1
    #endif
#endif

static inline int shared_stat_load(const SharedStat *entry) {
#ifdef MCFISH_WIDE_ATOMIC_LOAD_ASM
    static_assert(sizeof(SharedStat) == sizeof(int16_t),
                  "the operand below reads exactly the two bytes the entry occupies");
    static_assert(alignof(SharedStat) == alignof(int16_t),
                  "an entry must be naturally aligned for a plain load to be atomic");
    static_assert(ATOMIC_SHORT_LOCK_FREE == 2, "a locked atomic is not a plain load");

    int value;
    __asm__("movswl %1, %0" : "=r"(value) : "m"(*entry));
    return value;
#else
    return atomic_load_explicit(entry, memory_order_relaxed);
#endif
}

static inline void shared_stat_store(SharedStat *entry, int16_t value) {
    atomic_store_explicit(entry, value, memory_order_relaxed);
}

// Update ENTRY by gravity toward [-D, D], as `stats_update` does, through relaxed atomic
// accesses. This is NOT an atomic read-modify-write, and upstream's `operator<<` on a
// RelaxedAtomic is not one either: two workers updating one entry can lose one of the two
// updates. That is upstream's behaviour, not a bug to repair here -- an atomic RMW would
// serialise the hottest write in the engine to buy a heuristic no one reads exactly.
static inline void shared_stats_update(SharedStat *entry, int bonus, HistLimit d) {
    const int lim = d.v;
    const int clamped = bonus < -lim ? -lim : (bonus > lim ? lim : bonus);
    const int val = shared_stat_load(entry);
    const int abs_clamped = clamped < 0 ? -clamped : clamped;
    shared_stat_store(entry, (int16_t) (val + clamped - val * abs_clamped / lim));
}

// Bundle the four correction entries stored per (key, color) slot.
//
// Each field has its OWN type, and that is the guarantee rather than decoration.
// The four counters are selected by four DIFFERENT Zobrist keys, so a reader that
// takes the row from one key and the field for another gets a real counter of the
// wrong kind — a plausible wrong correction, never a fault. Four one-member
// structs make those four pointers mutually incompatible, so the mispairing is a
// compile error at the consumer instead of a silent read.
//
// A one-member struct is C's only true newtype: unlike a `typedef enum : T`, which
// clang diagnoses as a warning, a distinct struct type is a hard error with no
// flag involved. It costs nothing — the asserts below pin the layout, and the bulk
// clear in `history.c` reads the whole array through one `int16_t *` regardless.
typedef struct {
    SharedStat v;
} CorrPawnStat;
typedef struct {
    SharedStat v;
} CorrMinorStat;
typedef struct {
    SharedStat v;
} CorrNonPawnWhiteStat;
typedef struct {
    SharedStat v;
} CorrNonPawnBlackStat;

typedef struct {
    CorrPawnStat pawn;
    CorrMinorStat minor;
    CorrNonPawnWhiteStat nonpawn_white;
    CorrNonPawnBlackStat nonpawn_black;
} CorrectionBundle;

// Wrapping must not move a byte: `shared_histories_clear` zeroes the whole
// correction array through a single `int16_t *`, which is only valid while the
// bundle is exactly four counters end to end.
static_assert(sizeof(CorrectionBundle) == 4 * sizeof(SharedStat),
              "a correction bundle is four counters, unpadded");
static_assert(alignof(CorrectionBundle) == alignof(SharedStat),
              "wrapping a counter must not change its alignment");
static_assert(offsetof(CorrectionBundle, pawn) == 0 * sizeof(SharedStat),
              "the pawn counter leads the bundle");
static_assert(offsetof(CorrectionBundle, minor) == 1 * sizeof(SharedStat),
              "the minor counter is second");
static_assert(offsetof(CorrectionBundle, nonpawn_white) == 2 * sizeof(SharedStat),
              "the white non-pawn counter is third");
static_assert(offsetof(CorrectionBundle, nonpawn_black) == 3 * sizeof(SharedStat),
              "the black non-pawn counter is fourth");

// Hold the tables the workers of ONE NUMA node share: the key-indexed correction and
// pawn tables, whose sizes scale with that node's thread count, and the continuation
// block, whose size does not. Upstream keeps all three in `SharedHistories` and hands
// every Worker a reference (search.h:341); only the tables above stay per-worker.
//
// The two masks are the sizes minus one and the sizes are powers of two, so an index is
// a mask and never a modulo. Binding a pointer without its mask is what turns a wrapped
// index into an out-of-range read, so `shared_histories_create` is the only writer.
//
// Upstream: history.h:202 (SharedHistories), history.h:95 (DynStats).
typedef struct SharedHistories {
    size_t corr_size;
    size_t corr_size_minus1;
    CorrectionBundle (*correction_history)[COLOR_NB];

    size_t pawn_size;
    size_t pawn_size_minus1;
    SharedStat *pawn_history;

    SharedStat *continuation_history;  // CONTINUATION_PAGES * HIST_PIECETO entries
} SharedHistories;

// Allocate one node's bank sized for THREAD_COUNT threads, or null. THREAD_COUNT must be
// a power of two and at least 1 -- upstream asserts exactly that (history.h:205), because
// the size multiplier is what keeps the index a mask.
[[nodiscard]] SharedHistories *shared_histories_create(size_t thread_count);
void shared_histories_destroy(SharedHistories *sh);

// Hold one worker's own tables plus the reference to its node's bank. `shared` is never
// null once the worker is constructed; every accessor below dereferences it.
//
// The five fields after `shared` are copies of the bank's hot fields, taken by
// histories_bind_shared when the reference is bound. The hot accessors read the
// copies, so a per-node table access costs one load from this block instead of
// the dependent `h->shared` chain -- upstream's Worker reads its DynStats
// base+mask through one indirection (search.h:341, history.h:95), and the chain
// was a second one the port had added on top.
typedef struct Histories {
    int16_t main_history[COLOR_NB * HIST_UINT16];
    int16_t low_ply_history[LOW_PLY_HISTORY_SIZE * HIST_UINT16];
    int16_t capture_history[PIECE_NB * SQUARE_NB * HIST_PIECE_TYPE_NB];
    int16_t continuation_correction_history[HIST_PIECETO * HIST_PIECETO];
    int16_t tt_move_history;
    SharedHistories *shared;
    CorrectionBundle (*corr_base)[COLOR_NB];  // shared->correction_history
    size_t corr_mask;                         // shared->corr_size_minus1
    SharedStat *pawn_base;                    // shared->pawn_history
    size_t pawn_mask;                         // shared->pawn_size_minus1
    SharedStat *cont_base;                    // shared->continuation_history
} Histories;

// Bind SH as H's bank and refresh the cached base/mask copies. The two writers of
// `shared` (worker construction, the headless block) must go through this, or the
// hot accessors read a stale bank.
static inline void histories_bind_shared(Histories *h, SharedHistories *sh) {
    h->shared = sh;
    h->corr_base = sh != nullptr ? sh->correction_history : nullptr;
    h->corr_mask = sh != nullptr ? sh->corr_size_minus1 : 0;
    h->pawn_base = sh != nullptr ? sh->pawn_history : nullptr;
    h->pawn_mask = sh != nullptr ? sh->pawn_size_minus1 : 0;
    h->cont_base = sh != nullptr ? sh->continuation_history : nullptr;
}

// Return the engine's single history block, bound to a single-thread shared bank. It is
// the block the netless / headless callers -- the unit tests and any `search_go` driven
// without a pool -- run against. Return null when the bank could not be allocated.
Histories *histories(void);

// Release the process-wide block's shared bank. Call once at process exit; the next
// `histories()` rebuilds it.
void histories_shutdown(void);

// Mirror one search-stack frame. `continuation_history` points at a
// Update ENTRY by gravity toward [-D, D]. D must not exceed INT16_MAX.
static inline void stats_update(int16_t *entry, int bonus, HistLimit d) {
    const int lim = d.v;
    const int clamped = bonus < -lim ? -lim : (bonus > lim ? lim : bonus);
    const int val = *entry;
    const int abs_clamped = clamped < 0 ? -clamped : clamped;
    *entry = (int16_t) (val + clamped - val * abs_clamped / lim);
}

// Return the continuation-history page do_move installs on the stack:
// continuationHistory[in_check][capture][pc][to]. The null move and the
// iterative-deepening sentinels pass all-zero indices and land on the table base.
static inline SharedStat *
cont_hist_page(Histories *h, bool in_check, bool capture, Piece pc, Square to) {
    const size_t block = ((size_t) in_check * 2 + (size_t) capture) * HIST_PIECETO
                       + (size_t) pc * SQUARE_NB + (size_t) to;
    return &h->cont_base[block * HIST_PIECETO];
}

// Return the continuation-correction page for (pc, to).
static inline int16_t *cont_corr_page(Histories *h, Piece pc, Square to) {
    const size_t block = (size_t) pc * SQUARE_NB + (size_t) to;
    return &h->continuation_correction_history[block * HIST_PIECETO];
}

// Return the pawn-history page for PAWN_KEY: HIST_PIECETO entries, pc * 64 + to.
static inline SharedStat *pawn_history_row(Histories *h, Key pawn_key) {
    const size_t idx = (size_t) pawn_key & h->pawn_mask;
    return &h->pawn_base[idx * HIST_PIECETO];
}

// Return correctionHistory[key & mask][us]. NOT for call sites: the four counters
// in the row are selected by four DIFFERENT keys, so a caller that picks the row
// and the field separately can pair them wrongly. Use the four accessors below,
// each of which reads its own key and its own field.
static inline CorrectionBundle *corr_row(Histories *h, Key key, Color us) {
    const size_t idx = (size_t) key & h->corr_mask;
    return &h->corr_base[idx][us];
}

// Return the correction counter each key selects — one accessor per (key, field)
// pair, reading BOTH halves itself.
//
// The pairing used to be a convention restated by hand at eight call sites, and
// `corr_bundle(h, st->pawn_key, us)->minor` compiled: a real counter of the wrong
// kind, so a plausible wrong correction rather than a fault. No value gate can
// see it — every gate here compares output THIS binary produced, so they agree
// unanimously and are wrong together — and the bench signature says only that
// something moved, never where.
//
// Neither the key nor the field is a parameter now, so a call site holds nothing
// to transpose, and the field types make the old shape a compile error even though
// C leaves `corr_row` visible. What this does NOT close is stated in
// docs/09-type-design.md: all four accessors share a signature, so they remain
// swappable for each other, and only the bench signature catches that.
static inline SharedStat *corr_pawn_entry(Histories *h, const StateInfo *st, Color us) {
    return &corr_row(h, st->pawn_key, us)->pawn.v;
}
static inline SharedStat *corr_minor_entry(Histories *h, const StateInfo *st, Color us) {
    return &corr_row(h, st->minor_piece_key, us)->minor.v;
}
static inline SharedStat *corr_nonpawn_white_entry(Histories *h, const StateInfo *st, Color us) {
    return &corr_row(h, st->non_pawn_key[WHITE], us)->nonpawn_white.v;
}
static inline SharedStat *corr_nonpawn_black_entry(Histories *h, const StateInfo *st, Color us) {
    return &corr_row(h, st->non_pawn_key[BLACK], us)->nonpawn_black.v;
}

// Return &captureHistory[pc][to][captured_pt].
static inline int16_t *capture_entry(Histories *h, Piece pc, Square to, PieceType captured) {
    return &h->capture_history[(size_t) pc * (SQUARE_NB * HIST_PIECE_TYPE_NB)
                               + (size_t) to * HIST_PIECE_TYPE_NB + (size_t) captured];
}

// Reset one worker's tables and ITS STRIPE of the node's shared tables, exactly as
// upstream's Worker::clear does (search.cpp:676). NUMA_THREAD_IDX is this worker's index
// within its NUMA node and NUMA_TOTAL that node's worker count; a NUMA_TOTAL of 0 is read
// as 1. The stripe is `[i * n / total, (i + 1) * n / total)`, so with one worker the slice
// is the whole table -- and with several, every entry falls in exactly one worker's slice.
//
// The shared CONTINUATION block is deliberately NOT striped: upstream has every worker
// fill all of it (search.cpp:690-694), so the clear is a benign write race by design and
// striping it would be a divergence, not a fix.
//
// Call once per `ucinewgame`, on every worker.
void history_clear(Histories *h, size_t numa_thread_idx, size_t numa_total);

// Decay the main history once per search (before the ID loop): v * 729 / 1024.
void history_age_main(Histories *h);

// Refill the low-ply history with 102, once per search.
void history_fill_low_ply(Histories *h);

// Nudge the four key-indexed correction tables and the (ss-2)/(ss-4) continuation
// corrections toward the search / static-eval delta. Take exactly the three stack
// facts the update reads — the previous move and the two continuation-correction
// pages — so the caller gathers nothing the update discards.
//
// The four Zobrist keys are NOT a parameter. They used to arrive as a bundle the
// caller copied out of `pos->st` immediately before the call, which put four keys
// and four fields in one function for the update to pair by hand. It reads each
// key through that key's own accessor instead.
void history_update_correction(Histories *h,
                               const Position *pos,
                               Color us,
                               Move prev_move,
                               int16_t *cont_corr2,
                               int16_t *cont_corr4,
                               int bonus);

#endif  // MCFISH_HISTORY_H
