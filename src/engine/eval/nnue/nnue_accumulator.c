// Implement the incremental NNUE accumulator: the arena layout, the weight-row add/sub
// kernels, the refresh cache, the refresh/incremental update algorithm, and the
// transform. See nnue_accumulator.h for the incremental invariant.

#include "nnue_accumulator.h"

#include <assert.h>
#include <string.h>

#include "simd.h"

// ---------------------------------------------------------------------------
// Arena layout
// ---------------------------------------------------------------------------

// Name the two feature sets by their storage slot. The single COMBINED accumulator
// (HalfKA + Threats) lives in the psq slot; the threat slot carries only that feature
// set's per-ply diff. This is upstream's apply_combined shape, not two accumulators.
enum { PSQ_FEATURE = 0, THREAT_FEATURE = 1 };

enum : size_t {
    ACC_BYTES = NNUE_COLOR_COUNT * NNUE_HALF_DIMENSIONS * sizeof(int16_t)
              + NNUE_COLOR_COUNT * NNUE_PSQT_BUCKETS * sizeof(int32_t)
              + NNUE_COLOR_COUNT * sizeof(bool),
    COMPUTED_OFFSET = NNUE_COLOR_COUNT * NNUE_HALF_DIMENSIONS * sizeof(int16_t)
                    + NNUE_COLOR_COUNT * NNUE_PSQT_BUCKETS * sizeof(int32_t),
    PSQT_OFFSET = NNUE_COLOR_COUNT * NNUE_HALF_DIMENSIONS * sizeof(int16_t),

    ACC_STATE_BYTES = NNUE_ROUND_UP(ACC_BYTES, NNUE_ALIGN),
    PSQ_DIFF_OFFSET = ACC_BYTES,
    THREAT_DIFF_OFFSET = NNUE_ROUND_UP(ACC_BYTES, alignof(NnueDirtyThreats)),
    PSQ_STATE_STRIDE = ACC_STATE_BYTES,
    THREAT_STATE_STRIDE = NNUE_ROUND_UP(THREAT_DIFF_OFFSET + sizeof(NnueDirtyThreats), NNUE_ALIGN),
    PSQ_ARRAY_BYTES = PSQ_STATE_STRIDE * NNUE_MAX_STACK_SIZE,
    THREAT_ARRAY_OFFSET = PSQ_ARRAY_BYTES,
    THREAT_ARRAY_BYTES = THREAT_STATE_STRIDE * NNUE_MAX_STACK_SIZE,
    STACK_SIZE_OFFSET = THREAT_ARRAY_OFFSET + THREAT_ARRAY_BYTES,
    STACK_BYTES = STACK_SIZE_OFFSET + sizeof(size_t),
    THREAT_REFRESH_DIFF_OFFSET = THREAT_DIFF_OFFSET + sizeof(NnueDirtyThreatList),
};

// Assert what the accessors' pointer casts assume. Every state base is reached as
// `base + stride * index`, so each stride must carry the arena's 64-byte alignment
// forward or the int16/int32 views below are unaligned — which x86 tolerates and aarch64
// does not. These are arithmetic facts today; pin them so a change to
// NNUE_HALF_DIMENSIONS, NNUE_PSQT_BUCKETS or NNUE_MAX_STACK_SIZE fails the build instead
// of the target.
static_assert(PSQ_STATE_STRIDE % NNUE_ALIGN == 0, "psq stride must keep the arena alignment");
static_assert(THREAT_STATE_STRIDE % NNUE_ALIGN == 0, "threat stride must keep the arena alignment");
static_assert(THREAT_ARRAY_OFFSET % NNUE_ALIGN == 0, "threat array must keep the arena alignment");
static_assert(THREAT_DIFF_OFFSET % alignof(NnueDirtyThreats) == 0,
              "the threat diff offset must satisfy NnueDirtyThreats' alignment");
// PSQ_DIFF_OFFSET is deliberately NOT rounded: NnueDirtyPiece is all-uint8_t, so it needs
// no alignment. Pin that, since rounding it would silently move every psq diff.
static_assert(alignof(NnueDirtyPiece) == 1, "NnueDirtyPiece must stay alignment-free");
// The threat refresh test reads us/prev_ksq/ksq at THREAT_REFRESH_DIFF_OFFSET + 0/1/2, so
// that offset must land on the trailing scalars, not inside the list.
static_assert(THREAT_REFRESH_DIFF_OFFSET - THREAT_DIFF_OFFSET == offsetof(NnueDirtyThreats, us),
              "the threat refresh offset must address NnueDirtyThreats.us");
static_assert(offsetof(NnueDirtyThreats, prev_ksq) == offsetof(NnueDirtyThreats, us) + 1, "");
static_assert(offsetof(NnueDirtyThreats, ksq) == offsetof(NnueDirtyThreats, us) + 2, "");

size_t nnue_accumulator_stack_bytes(void) { return STACK_BYTES; }

// --- refresh-cache layout ---------------------------------------------------------

enum : size_t {
    CACHE_ENTRY_PSQT_OFFSET = NNUE_HALF_DIMENSIONS * sizeof(int16_t),
    CACHE_ENTRY_PIECES_OFFSET = CACHE_ENTRY_PSQT_OFFSET + NNUE_PSQT_BUCKETS * sizeof(int32_t),
    CACHE_ENTRY_PIECE_BB_OFFSET = CACHE_ENTRY_PIECES_OFFSET + SQUARE_NB * sizeof(uint8_t),
    CACHE_ENTRY_BYTES = NNUE_ROUND_UP(CACHE_ENTRY_PIECE_BB_OFFSET + sizeof(uint64_t), NNUE_ALIGN),
    CACHE_BYTES = SQUARE_NB * NNUE_COLOR_COUNT * CACHE_ENTRY_BYTES,
};

size_t nnue_refresh_cache_bytes(void) { return CACHE_BYTES; }

// ---------------------------------------------------------------------------
// Weight-row kernels
// ---------------------------------------------------------------------------

// Set the lane count for the feature-transformer weight-row add/sub tile. Independent of
// TRANSFORM_VEC_WIDTH below — they touch different loops, and a sweep of each finds
// different optima. Do not fold them into one knob. The avx512 build sweeps to 128, where
// vpaddw over a zmm register pair halves the loop trips; the narrower tiers keep 64, since a
// 128-lane u16 tile spills 16 xmm / 8 ymm registers and loses more than the trip count saves.
#if defined(__AVX512F__)
enum { ROW_TILE_WIDTH = 128 };
    #define RowVecU16 NnueV128u16
    #define row_load nnue_v128u16_load
    #define row_store nnue_v128u16_store
    #define row_add nnue_v128u16_add
    #define row_sub nnue_v128u16_sub
    #define row_i8_load nnue_v128i8_load
    #define row_i8_to_i16 nnue_v128_i8_to_i16
    #define row_i16_as_u16 nnue_v128_i16_as_u16
#else
enum { ROW_TILE_WIDTH = 64 };
    #define RowVecU16 NnueV64u16
    #define row_load nnue_v64u16_load
    #define row_store nnue_v64u16_store
    #define row_add nnue_v64u16_add
    #define row_sub nnue_v64u16_sub
    #define row_i8_load nnue_v64i8_load
    #define row_i8_to_i16 nnue_v64_i8_to_i16
    #define row_i16_as_u16 nnue_v64_i16_as_u16
#endif
static_assert(NNUE_HALF_DIMENSIONS % ROW_TILE_WIDTH == 0,
              "NNUE_HALF_DIMENSIONS must be a multiple of ROW_TILE_WIDTH");

// Set the lane count for the transform's clipped-ReLU pass.
enum { TRANSFORM_VEC_WIDTH = 64 };

// Widen an int8 weight row tile to the int16 accumulator's lane width, carried as
// uint16_t so the accumulation wraps rather than overflowing (see simd.h).
static inline RowVecU16 load_threat_row(const int8_t *w) {
    return row_i16_as_u16(row_i8_to_i16(row_i8_load(w)));
}

static inline RowVecU16 load_psq_row(const int16_t *w) { return row_load((const uint16_t *) w); }

static void acc_rows_i8(
  bool add, int16_t *target, const uint32_t *rows, size_t row_count, const int8_t *weights) {
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (target + d));
        for (size_t r = 0; r < row_count; r++) {
            const RowVecU16 w =
              load_threat_row(weights + (size_t) rows[r] * NNUE_HALF_DIMENSIONS + d);
            acc = add ? row_add(acc, w) : row_sub(acc, w);
        }
        row_store((uint16_t *) (target + d), acc);
    }
}

static void apply_delta_in_place_i16(int16_t *target,
                                     const uint32_t *removed,
                                     size_t removed_len,
                                     const uint32_t *added,
                                     size_t added_len,
                                     const int16_t *weights) {
    // Fuse removed and added into ONE tile sweep. Two calls to acc_rows_i16 loaded
    // and stored the whole accumulator twice -- the row-inner tiling saves the
    // per-row traffic but the removed/added split threw half of that back, so the
    // update read and wrote NNUE_HALF_DIMENSIONS twice over. This is upstream's
    // actual apply_combined shape (nnue_feature_transformer.h): hold the tile in a
    // register, subtract every removed row and add every added row into it, store
    // once.
    //
    // Bit-identical: (target - sum(removed)) + sum(added) is the same value however
    // the terms are grouped, and uint16 wrap addition is associative and
    // commutative, so no per-element order changes.
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (target + d));
        for (size_t r = 0; r < removed_len; r++)
            acc =
              row_sub(acc, load_psq_row(weights + (size_t) removed[r] * NNUE_HALF_DIMENSIONS + d));
        for (size_t r = 0; r < added_len; r++)
            acc =
              row_add(acc, load_psq_row(weights + (size_t) added[r] * NNUE_HALF_DIMENSIONS + d));
        row_store((uint16_t *) (target + d), acc);
    }
}

static void
accumulate_rows_i8(int16_t *target, const uint32_t *rows, size_t row_count, const int8_t *weights) {
    acc_rows_i8(true, target, rows, row_count, weights);
}

// Apply the psqt delta with the 8-bucket i32 row held in ONE register across every row,
// as the fused combined path does -- the scalar 8-step loop these replaced stayed scalar
// (the toolchain does not auto-vectorize integer loops). Per-row order (removed then
// added) is unchanged, so scalar==vector holds. Port of zfish ab086fd1e.
static_assert(NNUE_PSQT_BUCKETS == 8, "the psqt register width assumes 8 buckets");

static void apply_psqt_delta_in_place(int32_t *target,
                                      const uint32_t *removed,
                                      size_t removed_len,
                                      const uint32_t *added,
                                      size_t added_len,
                                      const int32_t *weights) {
    NnueV8i32 acc = nnue_v8i32_load(target);
    for (size_t i = 0; i < removed_len; i++)
        acc =
          nnue_v8i32_sub(acc, nnue_v8i32_load(weights + (size_t) removed[i] * NNUE_PSQT_BUCKETS));
    for (size_t i = 0; i < added_len; i++)
        acc = nnue_v8i32_add(acc, nnue_v8i32_load(weights + (size_t) added[i] * NNUE_PSQT_BUCKETS));
    nnue_v8i32_store(target, acc);
}

static void accumulate_psqt_rows(int32_t *target,
                                 const uint32_t *rows,
                                 size_t row_count,
                                 const int32_t *weights) {
    NnueV8i32 acc = nnue_v8i32_load(target);
    for (size_t i = 0; i < row_count; i++)
        acc = nnue_v8i32_add(acc, nnue_v8i32_load(weights + (size_t) rows[i] * NNUE_PSQT_BUCKETS));
    nnue_v8i32_store(target, acc);
}

// Port upstream's `apply_combined` (nnue_accumulator.cpp): ONE combined accumulator
// (HalfKA + Threats), loaded per tile once into a register, with both feature sets'
// removed/added weight rows applied in-register (psq int16 rows directly, threat int8
// rows widened), then stored once. Integer add/sub commute under two's-complement wrap,
// so the final tile value equals source + Σpsq_added − Σpsq_removed + Σthr_added −
// Σthr_removed regardless of order.
static void apply_combined_delta(int16_t *target,
                                 const int16_t *source,
                                 const uint32_t *psq_removed,
                                 size_t psq_removed_len,
                                 const uint32_t *psq_added,
                                 size_t psq_added_len,
                                 const uint32_t *thr_removed,
                                 size_t thr_removed_len,
                                 const uint32_t *thr_added,
                                 size_t thr_added_len,
                                 const int16_t *psq_weights,
                                 const int8_t *thr_weights) {
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (source + d));
        for (size_t i = 0; i < psq_removed_len; i++) {
            acc = row_sub(
              acc, load_psq_row(psq_weights + (size_t) psq_removed[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < psq_added_len; i++) {
            acc = row_add(
              acc, load_psq_row(psq_weights + (size_t) psq_added[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < thr_removed_len; i++) {
            acc = row_sub(acc, load_threat_row(
                                 thr_weights + (size_t) thr_removed[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < thr_added_len; i++) {
            acc = row_add(
              acc, load_threat_row(thr_weights + (size_t) thr_added[i] * NNUE_HALF_DIMENSIONS + d));
        }
        row_store((uint16_t *) (target + d), acc);
    }
}

// Mirror apply_combined_delta for psqt: one combined psqt accumulation, both feature sets
// applied. Scalar — NNUE_PSQT_BUCKETS is tiny.
static void apply_combined_psqt_delta(int32_t *target,
                                      const int32_t *source,
                                      const uint32_t *psq_removed,
                                      size_t psq_removed_len,
                                      const uint32_t *psq_added,
                                      size_t psq_added_len,
                                      const uint32_t *thr_removed,
                                      size_t thr_removed_len,
                                      const uint32_t *thr_added,
                                      size_t thr_added_len,
                                      const int32_t *psq_weights,
                                      const int32_t *thr_weights) {
    // Hold the 8 psqt buckets in a local so clang keeps them register-resident across
    // all four lists, instead of a memcpy to `target` then two more passes each
    // reloading target[] through a pointer whose aliasing blocks the optimizer. One
    // load from source, one store to target; upstream's register-held psqt path.
    int32_t acc[NNUE_PSQT_BUCKETS];
    for (size_t b = 0; b < NNUE_PSQT_BUCKETS; b++)
        acc[b] = source[b];
    for (size_t i = 0; i < psq_removed_len; i++) {
        const size_t row = (size_t) psq_removed[i] * NNUE_PSQT_BUCKETS;
        for (size_t b = 0; b < NNUE_PSQT_BUCKETS; b++)
            acc[b] -= psq_weights[row + b];
    }
    for (size_t i = 0; i < psq_added_len; i++) {
        const size_t row = (size_t) psq_added[i] * NNUE_PSQT_BUCKETS;
        for (size_t b = 0; b < NNUE_PSQT_BUCKETS; b++)
            acc[b] += psq_weights[row + b];
    }
    for (size_t i = 0; i < thr_removed_len; i++) {
        const size_t row = (size_t) thr_removed[i] * NNUE_PSQT_BUCKETS;
        for (size_t b = 0; b < NNUE_PSQT_BUCKETS; b++)
            acc[b] -= thr_weights[row + b];
    }
    for (size_t i = 0; i < thr_added_len; i++) {
        const size_t row = (size_t) thr_added[i] * NNUE_PSQT_BUCKETS;
        for (size_t b = 0; b < NNUE_PSQT_BUCKETS; b++)
            acc[b] += thr_weights[row + b];
    }
    for (size_t b = 0; b < NNUE_PSQT_BUCKETS; b++)
        target[b] = acc[b];
}

// ---------------------------------------------------------------------------
// Arena accessors
// ---------------------------------------------------------------------------

static inline const unsigned char *stack_bytes(const NnueAccumulatorStack *stack) {
    return (const unsigned char *) stack;
}

static inline unsigned char *stack_bytes_mut(NnueAccumulatorStack *stack) {
    return (unsigned char *) stack;
}

static size_t stack_size(const NnueAccumulatorStack *stack) {
    size_t size;
    memcpy(&size, stack_bytes(stack) + STACK_SIZE_OFFSET, sizeof size);
    return size;
}

static void set_stack_size(unsigned char *bytes, size_t size) {
    memcpy(bytes + STACK_SIZE_OFFSET, &size, sizeof size);
}

static size_t state_offset(int feature_kind, size_t index) {
    return feature_kind == PSQ_FEATURE ? index * PSQ_STATE_STRIDE
                                       : THREAT_ARRAY_OFFSET + index * THREAT_STATE_STRIDE;
}

static size_t diff_offset(int feature_kind) {
    return feature_kind == PSQ_FEATURE ? PSQ_DIFF_OFFSET : THREAT_DIFF_OFFSET;
}

static const unsigned char *
state_bytes_const(int feature_kind, size_t index, const NnueAccumulatorStack *stack) {
    return stack_bytes(stack) + state_offset(feature_kind, index);
}

static unsigned char *state_bytes_mut(int feature_kind, size_t index, NnueAccumulatorStack *stack) {
    return stack_bytes_mut(stack) + state_offset(feature_kind, index);
}

static bool state_computed(const NnueAccumulatorStack *stack,
                           int feature_kind,
                           size_t index,
                           uint8_t perspective) {
    return state_bytes_const(feature_kind, index, stack)[COMPUTED_OFFSET + perspective] != 0;
}

static void clear_computed(unsigned char *bytes, int feature_kind, size_t index) {
    memset(bytes + state_offset(feature_kind, index) + COMPUTED_OFFSET, 0, NNUE_COLOR_COUNT);
}

// Report whether the ply at INDEX moved PERSPECTIVE's own king. Reads the diff's first
// byte, which is NnueDirtyPiece.pc — see the alignment assert above.
static bool psq_requires_refresh(const unsigned char *bytes, size_t index, uint8_t perspective) {
    const size_t offset = state_offset(PSQ_FEATURE, index) + PSQ_DIFF_OFFSET;
    return bytes[offset] == (uint8_t) (6 + 8 * perspective);
}

static bool threat_requires_refresh(const unsigned char *bytes, size_t index, uint8_t perspective) {
    const size_t offset = state_offset(THREAT_FEATURE, index) + THREAT_REFRESH_DIFF_OFFSET;
    return nnue_full_requires_refresh(bytes[offset], bytes[offset + 1], bytes[offset + 2],
                                      perspective);
}

static bool state_requires_refresh(const NnueAccumulatorStack *stack,
                                   int feature_kind,
                                   size_t index,
                                   uint8_t perspective) {
    return feature_kind == PSQ_FEATURE
           ? psq_requires_refresh(stack_bytes(stack), index, perspective)
           : threat_requires_refresh(stack_bytes(stack), index, perspective);
}

// Walk down to the newest slot that is either already computed or forces a refresh.
static size_t
find_last_usable(int feature_kind, const NnueAccumulatorStack *stack, uint8_t perspective) {
    for (size_t current = stack_size(stack) - 1; current > 0; current--) {
        if (state_computed(stack, feature_kind, current, perspective)) {
            return current;
        }
        if (state_requires_refresh(stack, feature_kind, current, perspective)) {
            return current;
        }
    }
    return 0;
}

static int16_t *state_accumulation_mut(int feature_kind,
                                       size_t index,
                                       NnueAccumulatorStack *stack,
                                       uint8_t perspective) {
    void *p = state_bytes_mut(feature_kind, index, stack)
            + perspective * NNUE_HALF_DIMENSIONS * sizeof(int16_t);
    return (int16_t *) p;
}

static const int16_t *state_accumulation_const(int feature_kind,
                                               size_t index,
                                               const NnueAccumulatorStack *stack,
                                               uint8_t perspective) {
    const void *p = state_bytes_const(feature_kind, index, stack)
                  + perspective * NNUE_HALF_DIMENSIONS * sizeof(int16_t);
    return (const int16_t *) p;
}

static int32_t *
state_psqt_mut(int feature_kind, size_t index, NnueAccumulatorStack *stack, uint8_t perspective) {
    void *p = state_bytes_mut(feature_kind, index, stack) + PSQT_OFFSET
            + perspective * NNUE_PSQT_BUCKETS * sizeof(int32_t);
    return (int32_t *) p;
}

static const int32_t *state_psqt_const(int feature_kind,
                                       size_t index,
                                       const NnueAccumulatorStack *stack,
                                       uint8_t perspective) {
    const void *p = state_bytes_const(feature_kind, index, stack) + PSQT_OFFSET
                  + perspective * NNUE_PSQT_BUCKETS * sizeof(int32_t);
    return (const int32_t *) p;
}

static unsigned char *diff_bytes_mut(int feature_kind, size_t index, NnueAccumulatorStack *stack) {
    return state_bytes_mut(feature_kind, index, stack) + diff_offset(feature_kind);
}

static NnueDirtyPiece psq_diff_at(const unsigned char *state) {
    NnueDirtyPiece diff;
    memcpy(&diff, state + PSQ_DIFF_OFFSET, sizeof diff);
    return diff;
}

static const NnueDirtyThreats *threat_diff_at(const unsigned char *state) {
    const void *p = state + THREAT_DIFF_OFFSET;
    return (const NnueDirtyThreats *) p;
}

// Compute upstream's `pos.square<KING>(c)` without depending on the board zone's
// king_square, which is typed in Square rather than the raw uint8_t the indexer wants.
static uint8_t nnue_king_square(const Position *pos, uint8_t color) {
    return (uint8_t) __builtin_ctzll(pos->by_color[color] & pos->by_type[KING]);
}

// ---------------------------------------------------------------------------
// Refresh cache
// ---------------------------------------------------------------------------

static unsigned char *
cache_entry(NnueRefreshCache *cache, uint8_t king_square, uint8_t perspective) {
    return (unsigned char *) cache
         + (((size_t) king_square * NNUE_COLOR_COUNT + perspective) * CACHE_ENTRY_BYTES);
}

static int16_t *cache_entry_accumulation(unsigned char *entry) {
    void *p = entry;
    return (int16_t *) p;
}

static int32_t *cache_entry_psqt(unsigned char *entry) {
    void *p = entry + CACHE_ENTRY_PSQT_OFFSET;
    return (int32_t *) p;
}

static uint8_t *cache_entry_pieces(unsigned char *entry) {
    return entry + CACHE_ENTRY_PIECES_OFFSET;
}

void nnue_clear_refresh_cache(NnueRefreshCache *cache, const int16_t *biases) {
    for (unsigned ks = 0; ks < SQUARE_NB; ks++) {
        for (unsigned p = 0; p < NNUE_COLOR_COUNT; p++) {
            unsigned char *entry = cache_entry(cache, (uint8_t) ks, (uint8_t) p);
            memcpy(entry, biases, NNUE_FT_BIASES_BYTES);
            memset(entry + CACHE_ENTRY_PSQT_OFFSET, 0, CACHE_ENTRY_BYTES - CACHE_ENTRY_PSQT_OFFSET);
        }
    }
}

// ---------------------------------------------------------------------------
// Refresh and incremental update
// ---------------------------------------------------------------------------

// Refresh the HalfKA half through the finny table: diff the cached board against the
// live one, apply that delta to the cached accumulation, then copy it into the slot.
static void set_cache_entry_piece_bb(unsigned char *entry, uint64_t piece_bb) {
    memcpy(entry + CACHE_ENTRY_PIECE_BB_OFFSET, &piece_bb, sizeof piece_bb);
}

static void refresh_latest_psq(uint8_t perspective,
                               uint8_t king_square,
                               NnueAccumulatorStack *stack,
                               const Position *pos,
                               const NnueFeatureTransformer *ft,
                               NnueRefreshCache *cache) {
    const size_t latest_index = stack_size(stack) - 1;
    unsigned char *entry = cache_entry(cache, king_square, perspective);
    uint8_t *entry_pieces = cache_entry_pieces(entry);
    const uint8_t *board = (const uint8_t *) pos->board;

    uint32_t removed[NNUE_PSQ_INDEX_CAPACITY];
    uint32_t added[NNUE_PSQ_INDEX_CAPACITY];
    size_t removed_len = 0;
    size_t added_len = 0;

    // One pass over the 64 squares: load entry/board and test old != new once, then
    // route the changed square to removed and/or added. Both lists are still built in
    // ascending-square order, so the applied delta is identical to the two-scan form.
    for (unsigned square = 0; square < SQUARE_NB; square++) {
        const uint8_t old_piece = entry_pieces[square];
        const uint8_t new_piece = board[square];
        if (old_piece == new_piece)
            continue;
        if (old_piece != NO_PIECE)
            removed[removed_len++] =
              nnue_half_make_index(perspective, (uint8_t) square, old_piece, king_square);
        if (new_piece != NO_PIECE)
            added[added_len++] =
              nnue_half_make_index(perspective, (uint8_t) square, new_piece, king_square);
    }

    apply_delta_in_place_i16(cache_entry_accumulation(entry), removed, removed_len, added,
                             added_len, nnue_ft_psq_weights(ft));
    apply_psqt_delta_in_place(cache_entry_psqt(entry), removed, removed_len, added, added_len,
                              nnue_ft_psq_psqt_weights(ft));

    memcpy(entry_pieces, board, SQUARE_NB);
    set_cache_entry_piece_bb(entry, pos->by_type[ALL_PIECES]);

    memcpy(state_accumulation_mut(PSQ_FEATURE, latest_index, stack, perspective),
           cache_entry_accumulation(entry), NNUE_HALF_DIMENSIONS * sizeof(int16_t));
    memcpy(state_psqt_mut(PSQ_FEATURE, latest_index, stack, perspective), cache_entry_psqt(entry),
           NNUE_PSQT_BUCKETS * sizeof(int32_t));
    state_bytes_mut(PSQ_FEATURE, latest_index, stack)[COMPUTED_OFFSET + perspective] = 1;
}

// Perform the fused refresh: the HalfKA half through the finny cache fills the combined
// accumulation and sets computed; the threat features are then ADDED on top (additive, no
// zeroing), so combined = psq + threat — the refresh half of apply_combined.
static void refresh_combined(uint8_t perspective,
                             uint8_t king_square,
                             NnueAccumulatorStack *stack,
                             const Position *pos,
                             const NnueFeatureTransformer *ft,
                             NnueRefreshCache *cache) {
    refresh_latest_psq(perspective, king_square, stack, pos, ft, cache);

    const size_t latest_index = stack_size(stack) - 1;
    NnueFullAppendResult active;
    nnue_full_append_active(perspective, king_square, (const uint8_t *) pos->board,
                            (const uint64_t *) pos->by_type, (const uint64_t *) pos->by_color,
                            &active);

    accumulate_rows_i8(state_accumulation_mut(PSQ_FEATURE, latest_index, stack, perspective),
                       active.indices, active.len, nnue_ft_threat_weights(ft));
    accumulate_psqt_rows(state_psqt_mut(PSQ_FEATURE, latest_index, stack, perspective),
                         active.indices, active.len, nnue_ft_threat_psqt_weights(ft));
}

static void append_half_change(uint32_t *removed,
                               size_t *removed_len,
                               uint32_t *added,
                               size_t *added_len,
                               uint32_t index,
                               bool is_removed) {
    if (is_removed) {
        removed[(*removed_len)++] = index;
    } else {
        added[(*added_len)++] = index;
    }
}

// Take one fused incremental step onto the combined accumulator — a port of upstream's
// update_accumulator_incremental + apply_combined. Compute the HalfKA and threat
// changed-feature index lists for this ply, then apply both to the single combined
// accumulation in one load/store per tile.
//
// FORWARD says which direction the step runs. Going forward, the diff belongs to the
// target slot and its `from`/`remove_sq` indices are removals; going backward the diff
// belongs to the computed slot and every role is inverted.
static void apply_combined(NnueAccumulatorStack *stack,
                           uint8_t perspective,
                           const NnueFeatureTransformer *ft,
                           uint8_t king_square,
                           size_t target_index,
                           size_t computed_index,
                           bool forward) {
    assert(state_computed(stack, PSQ_FEATURE, computed_index, perspective));
    assert(!state_computed(stack, PSQ_FEATURE, target_index, perspective));

    const NnueDirtyPiece psq_diff =
      psq_diff_at(state_bytes_const(PSQ_FEATURE, forward ? target_index : computed_index, stack));

    NnueHalfAppendResult psq_append;
    nnue_half_append_changed(perspective, king_square, psq_diff, &psq_append);

    uint32_t psq_removed[NNUE_PSQ_INDEX_CAPACITY];
    uint32_t psq_added[NNUE_PSQ_INDEX_CAPACITY];
    size_t psq_removed_len = 0;
    size_t psq_added_len = 0;
    size_t cursor = 0;

    append_half_change(psq_removed, &psq_removed_len, psq_added, &psq_added_len,
                       psq_append.indices[cursor], forward);
    cursor += 1;
    if (psq_diff.to != NNUE_SQ_NONE) {
        append_half_change(psq_removed, &psq_removed_len, psq_added, &psq_added_len,
                           psq_append.indices[cursor], !forward);
        cursor += 1;
    }
    if (psq_diff.remove_sq != NNUE_SQ_NONE) {
        append_half_change(psq_removed, &psq_removed_len, psq_added, &psq_added_len,
                           psq_append.indices[cursor], forward);
        cursor += 1;
    }
    if (psq_diff.add_sq != NNUE_SQ_NONE) {
        append_half_change(psq_removed, &psq_removed_len, psq_added, &psq_added_len,
                           psq_append.indices[cursor], !forward);
    }

    const NnueDirtyThreats *thr_diff = threat_diff_at(
      state_bytes_const(THREAT_FEATURE, forward ? target_index : computed_index, stack));

    NnueFullAppendResult thr_append;
    nnue_full_append_changed(perspective, king_square, thr_diff->list.values, thr_diff->list.size,
                             &thr_append);

    uint32_t thr_removed[NNUE_THREAT_INDEX_CAPACITY];
    uint32_t thr_added[NNUE_THREAT_INDEX_CAPACITY];
    size_t thr_removed_len = 0;
    size_t thr_added_len = 0;

    for (size_t list_index = 0; list_index < thr_append.len; list_index++) {
        const uint32_t index = thr_append.indices[list_index];
        if (index >= NNUE_FULL_DIMENSIONS) {
            continue;
        }
        const bool is_add =
          (thr_diff->list.values[list_index].data >> NNUE_DIRTY_THREAT_ADD_SHIFT) != 0;
        if (is_add == forward) {
            thr_added[thr_added_len++] = index;
        } else {
            thr_removed[thr_removed_len++] = index;
        }
    }

    apply_combined_delta(state_accumulation_mut(PSQ_FEATURE, target_index, stack, perspective),
                         state_accumulation_const(PSQ_FEATURE, computed_index, stack, perspective),
                         psq_removed, psq_removed_len, psq_added, psq_added_len, thr_removed,
                         thr_removed_len, thr_added, thr_added_len, nnue_ft_psq_weights(ft),
                         nnue_ft_threat_weights(ft));
    apply_combined_psqt_delta(state_psqt_mut(PSQ_FEATURE, target_index, stack, perspective),
                              state_psqt_const(PSQ_FEATURE, computed_index, stack, perspective),
                              psq_removed, psq_removed_len, psq_added, psq_added_len, thr_removed,
                              thr_removed_len, thr_added, thr_added_len,
                              nnue_ft_psq_psqt_weights(ft), nnue_ft_threat_psqt_weights(ft));
    state_bytes_mut(PSQ_FEATURE, target_index, stack)[COMPUTED_OFFSET + perspective] = 1;
}

// Walk the stack once for PERSPECTIVE over the combined accumulator — a port of upstream's
// AccumulatorStack::evaluate_side. find_last_usable consults ONLY the HalfKA refresh
// condition, because a threat refresh (a king move across the centre) is a subset of a
// HalfKA refresh, so the combined accumulator always refreshes together.
static void evaluate_side(uint8_t perspective,
                          NnueAccumulatorStack *stack,
                          const Position *pos,
                          const NnueFeatureTransformer *ft,
                          NnueRefreshCache *cache) {
    const size_t last_usable = find_last_usable(PSQ_FEATURE, stack, perspective);
    const size_t size = stack_size(stack);
    const uint8_t king_square = nnue_king_square(pos, perspective);

    if (state_computed(stack, PSQ_FEATURE, last_usable, perspective)) {
        for (size_t next = last_usable + 1; next < size; next++) {
            apply_combined(stack, perspective, ft, king_square, next, next - 1, true);
        }
    } else {
        refresh_combined(perspective, king_square, stack, pos, ft, cache);

        for (size_t computed_index = size - 1; computed_index > last_usable; computed_index--) {
            apply_combined(stack, perspective, ft, king_square, computed_index - 1, computed_index,
                           false);
        }
    }
}

void nnue_acc_evaluate(NnueAccumulatorStack *stack,
                       const Position *pos,
                       const NnueFeatureTransformer *ft,
                       NnueRefreshCache *cache) {
    // Match upstream AccumulatorStack::evaluate: one combined (HalfKA + Threats) pass per
    // perspective, not one per (feature, perspective).
    evaluate_side(WHITE, stack, pos, ft, cache);
    evaluate_side(BLACK, stack, pos, ft, cache);
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

int32_t nnue_transform_bucket(NnueAccumulatorStack *stack,
                              const Position *pos,
                              const NnueFeatureTransformer *ft,
                              NnueRefreshCache *cache,
                              size_t bucket,
                              uint8_t stm,
                              uint8_t *output,
                              NnueNnzBitset *nnz) {
    nnue_acc_evaluate(stack, pos, ft, cache);

    const size_t latest = stack_size(stack) - 1;
    const unsigned char *state = state_bytes_const(PSQ_FEATURE, latest, stack);
    const int16_t *comb_acc = (const int16_t *) (const void *) state;
    const int32_t *comb_psqt = (const int32_t *) (const void *) (state + PSQT_OFFSET);

    const size_t p0 = stm;
    const size_t p1 = stm ^ 1u;

    // (psq_diff + thr_diff)/2 == combined_diff/2, since combined = psq + threat.
    const int32_t psqt =
      (comb_psqt[p0 * NNUE_PSQT_BUCKETS + bucket] - comb_psqt[p1 * NNUE_PSQT_BUCKETS + bucket]) / 2;

    // Produce the pairwise squared-clipped-ReLU output (upstream FeatureTransformer::
    // transform). Per element: clip the accumulator into [0,255], multiply the two halves
    // and divide by 512 -> uint8. Stay in 16-bit via upstream's mulhi identity
    //   (c0*c1) >> 9  ==  ((c0<<7) * c1) >> 16,
    // which avoids the int32 widening so each register holds twice the lanes. The scaled
    // product 128*c0*c1 is exact and >>16 is floor, so this is bit-identical to the
    // int32 clamp*mul>>9 path — integer throughout, no rounding.
    const size_t half = NNUE_HALF_DIMENSIONS / 2;
    const NnueV64i16 zero = nnue_v64i16_splat(0);
    const NnueV64i16 c255 = nnue_v64i16_splat(255);

    memset(nnz, 0, sizeof(NnueNnzBitset));

    for (size_t p = 0; p < 2; p++) {
        const size_t pp = p == 0 ? p0 : p1;
        const size_t offset = half * p;
        const size_t base = pp * NNUE_HALF_DIMENSIONS;
        for (size_t j = 0; j < half; j += TRANSFORM_VEC_WIDTH) {
            const NnueV64i16 s0 = nnue_v64i16_load(comb_acc + base + j);
            const NnueV64i16 s1 = nnue_v64i16_load(comb_acc + base + j + half);
            const NnueV64u16 c0 =
              nnue_v64_i16_to_u16(nnue_v64i16_max(zero, nnue_v64i16_min(c255, s0)));
            const NnueV64u16 c1 =
              nnue_v64_i16_to_u16(nnue_v64i16_max(zero, nnue_v64i16_min(c255, s1)));
            const NnueV64u32 lhs = nnue_v64_u16_to_u32(nnue_v64u16_shl(c0, 7));
            const NnueV64u32 rhs = nnue_v64_u16_to_u32(c1);
            const NnueV64u16 q =
              nnue_v64_u32_to_u16(nnue_v64u32_shr(nnue_v64u32_mul(lhs, rhs), 16));
            const NnueV64u8 bytes = nnue_v64_u16_to_u8(q);
            nnue_v64u8_store(output + offset + j, bytes);

            // Record which 4-byte chunks are non-zero while they are still in a register:
            // no reload of what was just stored.
            const NnueV16u32 groups = nnue_v64_u8_as_u32x16(bytes);

            // Build the non-zero-chunk mask with one horizontal movemask (compare-to-zero
            // + reduce-OR) rather than sixteen per-lane extract+compare+shift+OR.
            const uint64_t mask = nnue_v16u32_movemask(groups);
            const size_t bit = (offset + j) / 4;
            (*nnz)[bit / 64] |= mask << (bit % 64);
        }
    }
    return psqt;
}

// ---------------------------------------------------------------------------
// Stack facade
// ---------------------------------------------------------------------------

static void zero_diff(unsigned char *bytes, int feature_kind, size_t index, size_t len) {
    memset(bytes + state_offset(feature_kind, index) + diff_offset(feature_kind), 0, len);
}

void nnue_acc_stack_reset(NnueAccumulatorStack *stack) {
    unsigned char *bytes = stack_bytes_mut(stack);

    clear_computed(bytes, PSQ_FEATURE, 0);
    zero_diff(bytes, PSQ_FEATURE, 0, sizeof(NnueDirtyPiece));

    clear_computed(bytes, THREAT_FEATURE, 0);
    zero_diff(bytes, THREAT_FEATURE, 0, sizeof(NnueDirtyThreats));

    set_stack_size(bytes, 1);
}

NnueStackPushOutput nnue_acc_stack_push(NnueAccumulatorStack *stack) {
    unsigned char *bytes = stack_bytes_mut(stack);
    const size_t index = stack_size(stack);
    assert(index < NNUE_MAX_STACK_SIZE);

    clear_computed(bytes, PSQ_FEATURE, index);
    clear_computed(bytes, THREAT_FEATURE, index);

    void *threat_slot = diff_bytes_mut(THREAT_FEATURE, index, stack);
    NnueDirtyThreats *dirty_threats = (NnueDirtyThreats *) threat_slot;
    dirty_threats->list.size = 0;

    set_stack_size(bytes, index + 1);

    void *psq_slot = diff_bytes_mut(PSQ_FEATURE, index, stack);
    return (NnueStackPushOutput) {
        .dirty_piece = (NnueDirtyPiece *) psq_slot,
        .dirty_threats = dirty_threats,
    };
}

void nnue_acc_stack_pop(NnueAccumulatorStack *stack) {
    unsigned char *bytes = stack_bytes_mut(stack);
    const size_t size = stack_size(stack);
    assert(size > 1);
    set_stack_size(bytes, size - 1);
}
