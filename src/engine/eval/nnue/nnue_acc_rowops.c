// Implement the weight-row add/sub kernels. See nnue_acc_rowops.h.

#include "nnue_acc_rowops.h"

#include "nnue_accumulator.h"  // NNUE_HALF_DIMENSIONS, NNUE_PSQT_BUCKETS
#include "simd.h"

// Set the lane count for the feature-transformer weight-row add/sub tile. The avx512
// build sweeps to 128, where vpaddw over a zmm register pair halves the loop trips; the
// narrower tiers keep 64, since a 128-lane u16 tile spills 16 xmm / 8 ymm registers and
// loses more than the trip count saves. The tile's type and ops are reached through
// width-selected macros so both tiers share one loop body.
#if defined(__AVX512F__)
enum { ROW_TILE_WIDTH = 128 };
    #define RowVecU16 NnueV128u16
    #define row_load nnue_v128u16_load_a
    #define row_store nnue_v128u16_store_a
    #define row_add nnue_v128u16_add
    #define row_sub nnue_v128u16_sub
    #define row_i8_load nnue_v128i8_load_a
    #define row_i8_to_i16 nnue_v128_i8_to_i16
    #define row_i16_as_u16 nnue_v128_i16_as_u16
#else
enum { ROW_TILE_WIDTH = 64 };
    #define RowVecU16 NnueV64u16
    #define row_load nnue_v64u16_load_a
    #define row_store nnue_v64u16_store_a
    #define row_add nnue_v64u16_add
    #define row_sub nnue_v64u16_sub
    #define row_i8_load nnue_v64i8_load_a
    #define row_i8_to_i16 nnue_v64_i8_to_i16
    #define row_i16_as_u16 nnue_v64_i16_as_u16
#endif
static_assert(NNUE_HALF_DIMENSIONS % ROW_TILE_WIDTH == 0,
              "NNUE_HALF_DIMENSIONS must be a multiple of ROW_TILE_WIDTH");

static_assert(NNUE_PSQT_BUCKETS == 8, "the psqt register width assumes 8 buckets");

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

void nnue_acc_apply_delta_i16(int16_t *target,
                              const uint32_t *removed,
                              size_t removed_len,
                              const uint32_t *added,
                              size_t added_len,
                              const int16_t *weights) {
    // Fuse removed and added into ONE tile sweep. This is upstream's apply_combined shape
    // (nnue_feature_transformer.h): hold the tile in a register, subtract every removed row
    // and add every added row into it, store once. Bit-identical: (target - sum(removed)) +
    // sum(added) is the same value however grouped, and uint16 wrap addition is associative
    // and commutative, so no per-element order changes.
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

// Dual-store refresh of nnue_acc_apply_delta_i16: same per-tile arithmetic, but the refreshed
// tile is stored to BOTH cache_dest (in place) and state_dest before moving on, so the value
// reaches the ply's state slot as a register store rather than a trailing 2KB memcpy. The value
// written to each destination is identical to the single-dest apply followed by a copy.
void nnue_acc_apply_delta_i16_dual(int16_t *cache_dest,
                                   int16_t *state_dest,
                                   const uint32_t *removed,
                                   size_t removed_len,
                                   const uint32_t *added,
                                   size_t added_len,
                                   const int16_t *weights) {
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (cache_dest + d));
        for (size_t r = 0; r < removed_len; r++)
            acc =
              row_sub(acc, load_psq_row(weights + (size_t) removed[r] * NNUE_HALF_DIMENSIONS + d));
        for (size_t r = 0; r < added_len; r++)
            acc =
              row_add(acc, load_psq_row(weights + (size_t) added[r] * NNUE_HALF_DIMENSIONS + d));
        row_store((uint16_t *) (cache_dest + d), acc);
        row_store((uint16_t *) (state_dest + d), acc);
    }
}

void nnue_acc_accumulate_rows_i8(int16_t *target,
                                 const uint32_t *rows,
                                 size_t row_count,
                                 const int8_t *weights) {
    acc_rows_i8(true, target, rows, row_count, weights);
}

// Apply the psqt delta with the 8-bucket i32 row held in ONE register across every row, as
// the fused combined path does -- the scalar 8-step loop these replaced stayed scalar (the
// toolchain does not auto-vectorize integer loops). Per-row order (removed then added) is
// unchanged, so scalar==vector holds. Port of zfish ab086fd1e.
void nnue_acc_apply_psqt_delta(int32_t *target,
                               const uint32_t *removed,
                               size_t removed_len,
                               const uint32_t *added,
                               size_t added_len,
                               const int32_t *weights) {
    NnueV8i32 acc = nnue_v8i32_load_a(target);
    for (size_t i = 0; i < removed_len; i++)
        acc =
          nnue_v8i32_sub(acc, nnue_v8i32_load_a(weights + (size_t) removed[i] * NNUE_PSQT_BUCKETS));
    for (size_t i = 0; i < added_len; i++)
        acc =
          nnue_v8i32_add(acc, nnue_v8i32_load_a(weights + (size_t) added[i] * NNUE_PSQT_BUCKETS));
    nnue_v8i32_store_a(target, acc);
}

// Dual-store refresh of nnue_acc_apply_psqt_delta: hold the 8 buckets in one register, then store
// the refreshed row to BOTH cache_dest and state_dest, fusing the cache→state copy. Bit-identical
// to the single-dest apply followed by a memcpy of the same buckets.
void nnue_acc_apply_psqt_delta_dual(int32_t *cache_dest,
                                    int32_t *state_dest,
                                    const uint32_t *removed,
                                    size_t removed_len,
                                    const uint32_t *added,
                                    size_t added_len,
                                    const int32_t *weights) {
    NnueV8i32 acc = nnue_v8i32_load_a(cache_dest);
    for (size_t i = 0; i < removed_len; i++)
        acc =
          nnue_v8i32_sub(acc, nnue_v8i32_load_a(weights + (size_t) removed[i] * NNUE_PSQT_BUCKETS));
    for (size_t i = 0; i < added_len; i++)
        acc =
          nnue_v8i32_add(acc, nnue_v8i32_load_a(weights + (size_t) added[i] * NNUE_PSQT_BUCKETS));
    nnue_v8i32_store_a(cache_dest, acc);
    nnue_v8i32_store_a(state_dest, acc);
}

void nnue_acc_accumulate_psqt_rows(int32_t *target,
                                   const uint32_t *rows,
                                   size_t row_count,
                                   const int32_t *weights) {
    NnueV8i32 acc = nnue_v8i32_load_a(target);
    for (size_t i = 0; i < row_count; i++)
        acc =
          nnue_v8i32_add(acc, nnue_v8i32_load_a(weights + (size_t) rows[i] * NNUE_PSQT_BUCKETS));
    nnue_v8i32_store_a(target, acc);
}

// Port upstream's `apply_combined` (nnue_accumulator.cpp): ONE combined accumulator (HalfKA +
// Threats), loaded per tile once into a register, with both feature sets' removed/added weight
// rows applied in-register (psq int16 rows directly, threat int8 rows widened), then stored
// once. Integer add/sub commute under two's-complement wrap, so the final tile value equals
// source + Σpsq_added − Σpsq_removed + Σthr_added − Σthr_removed regardless of order.
void nnue_acc_apply_combined_delta(int16_t *target,
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

// Mirror nnue_acc_apply_combined_delta for psqt: one combined psqt accumulation, both feature
// sets applied. Hold the 8 buckets in ONE NnueV8i32 register across all four lists -- as the
// sibling nnue_acc_apply_psqt_delta already does. Each feature's 8 buckets are contiguous, so a
// row is a single 256-bit load; the per-feature accumulator dependency keeps the outer loop
// sequential. Spelling the 8 buckets as a scalar array instead let clang's LTO vectorizer form
// the outer loops into strided vpgatherqd weight-row gathers (~5-12 cycles each on Zen4), which
// the register-resident vector form avoids. Per-row order (removed then added) is unchanged and
// integer add/sub commute under wrap, so scalar==vector holds.
void nnue_acc_apply_combined_psqt_delta(int32_t *target,
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
    NnueV8i32 acc = nnue_v8i32_load_a(source);
    for (size_t i = 0; i < psq_removed_len; i++)
        acc = nnue_v8i32_sub(
          acc, nnue_v8i32_load_a(psq_weights + (size_t) psq_removed[i] * NNUE_PSQT_BUCKETS));
    for (size_t i = 0; i < psq_added_len; i++)
        acc = nnue_v8i32_add(
          acc, nnue_v8i32_load_a(psq_weights + (size_t) psq_added[i] * NNUE_PSQT_BUCKETS));
    for (size_t i = 0; i < thr_removed_len; i++)
        acc = nnue_v8i32_sub(
          acc, nnue_v8i32_load_a(thr_weights + (size_t) thr_removed[i] * NNUE_PSQT_BUCKETS));
    for (size_t i = 0; i < thr_added_len; i++)
        acc = nnue_v8i32_add(
          acc, nnue_v8i32_load_a(thr_weights + (size_t) thr_added[i] * NNUE_PSQT_BUCKETS));
    nnue_v8i32_store_a(target, acc);
}
