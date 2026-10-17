// Implement the weight-row add/sub kernels. See nnue_acc_rowops.h.

#include "nnue_acc_rowops.h"

#include <assert.h>

#include "nnue_accumulator.h"  // NNUE_HALF_DIMENSIONS
#include "simd.h"

// Set the lane count for the feature-transformer weight-row add/sub tile, matching
// upstream SIMDTiling's budget of 8 native registers per tile at every x86 tier:
// 8 zmm = 256 u16 lanes on avx512, 8 ymm = 128 on avx2, 8 xmm = 64 on sse. A narrower
// tile re-walks each changed-row list once per extra tile pass, paying the loop
// control and per-row address setup again for the same math. The tile's type and ops
// are reached through width-selected macros so every tier shares one loop body.
#if defined(__AVX512F__)
enum { ROW_TILE_WIDTH = 256 };
    #define RowVecU16 NnueV256u16
    #define row_load nnue_v256u16_load_a
    #define row_store nnue_v256u16_store_a
    #define row_add nnue_v256u16_add
    #define row_sub nnue_v256u16_sub
    #define row_i8_load nnue_v256i8_load_a
    #define row_i8_to_i16 nnue_v256_i8_to_i16
    #define row_i16_as_u16 nnue_v256_i16_as_u16
#elif defined(__AVX2__)
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

// Widen int8 weight rows TWO at a time on the one tier where that does less work:
// SSSE3 has pmaddubsw but not SSE4.1's pmovsxbw, so the portable one-row widen lowers
// to an unpack and a shift per register, and pairing two rows' bytes into one unpack
// and one pmaddubsw against a vector of ones halves it -- upstream's
// USE_PAIRED_WIDENING_ADD. Upstream enables it for SSSE3 and better. Keep it off where
// SSE4.1 is: there the one-row widen is a pmovsxbw from memory and an add per
// register, which a pair at best matches -- and loses to under the two-operand SSE
// encoding, which copies the ones vector and an unpack input for every register.
//
// At 16 bytes the unpacks read a row's bytes in file order, so the weights need no
// load-time reorder for it (upstream's WideningPairOrder is the identity at this width).
#if MCFISH_SIMD_VECTOR && defined(__SSSE3__) && !defined(__SSE4_1__)
    #define ROW_PAIRED_WIDENING 1
#else
    #define ROW_PAIRED_WIDENING 0
#endif

#if !ROW_PAIRED_WIDENING
// Widen an int8 weight row tile to the int16 accumulator's lane width, carried as
// uint16_t so the accumulation wraps rather than overflowing (see simd.h).
static inline RowVecU16 load_row(const int8_t *w) {
    return row_i16_as_u16(row_i8_to_i16(row_i8_load(w)));
}

static void acc_rows_i8(
  bool add, int16_t *target, const uint32_t *rows, size_t row_count, const int8_t *weights) {
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (target + d));
        for (size_t r = 0; r < row_count; r++) {
            const RowVecU16 w = load_row(weights + (size_t) rows[r] * NNUE_HALF_DIMENSIONS + d);
            acc = add ? row_add(acc, w) : row_sub(acc, w);
        }
        row_store((uint16_t *) (target + d), acc);
    }
}

// Apply removed and added in ONE tile sweep -- upstream's apply_combined shape: hold the tile
// in a register, subtract every removed row and add every added row into it, then store the
// refreshed tile to BOTH cache_dest (in place) and state_dest before moving on, so the value
// reaches the ply's state slot as a register store rather than a trailing 2KB memcpy.
// Bit-identical however grouped: uint16 wrap addition is associative and commutative.
void nnue_acc_apply_delta_dual(int16_t *cache_dest,
                               int16_t *state_dest,
                               const uint32_t *removed,
                               size_t removed_len,
                               const uint32_t *added,
                               size_t added_len,
                               const int8_t *weights) {
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (cache_dest + d));
        for (size_t r = 0; r < removed_len; r++)
            acc = row_sub(acc, load_row(weights + (size_t) removed[r] * NNUE_HALF_DIMENSIONS + d));
        for (size_t r = 0; r < added_len; r++)
            acc = row_add(acc, load_row(weights + (size_t) added[r] * NNUE_HALF_DIMENSIONS + d));
        row_store((uint16_t *) (cache_dest + d), acc);
        row_store((uint16_t *) (state_dest + d), acc);
    }
}

void nnue_acc_apply_hybrid_delta(int16_t *target,
                                 const int16_t *source,
                                 int16_t *new_entry,
                                 const int16_t *old_entry,
                                 const uint32_t *new_removed,
                                 size_t new_removed_len,
                                 const uint32_t *new_added,
                                 size_t new_added_len,
                                 const uint32_t *old_removed,
                                 size_t old_removed_len,
                                 const uint32_t *old_added,
                                 size_t old_added_len,
                                 const uint32_t *thr_removed,
                                 size_t thr_removed_len,
                                 const uint32_t *thr_added,
                                 size_t thr_added_len,
                                 const int8_t *psq_weights,
                                 const int8_t *thr_weights) {
    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (new_entry + d));
        for (size_t i = 0; i < new_removed_len; i++) {
            acc = row_sub(
              acc, load_row(psq_weights + (size_t) new_removed[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < new_added_len; i++) {
            acc = row_add(acc,
                          load_row(psq_weights + (size_t) new_added[i] * NNUE_HALF_DIMENSIONS + d));
        }
        // The destination king's cache entry now describes the live board: store it
        // before the tile picks up terms that belong to this ply alone.
        row_store((uint16_t *) (new_entry + d), acc);

        // The previous ply's combined accumulation carries the threat/pair rows this
        // move mostly keeps, plus PSQ rows bucketed on the SOURCE king square.
        // Subtracting that king's cache entry and re-applying its own diff removes
        // exactly those, leaving the destination bucket's rows already added above.
        acc = row_add(acc, row_load((const uint16_t *) (source + d)));
        acc = row_sub(acc, row_load((const uint16_t *) (old_entry + d)));
        for (size_t i = 0; i < old_removed_len; i++) {
            acc = row_add(
              acc, load_row(psq_weights + (size_t) old_removed[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < old_added_len; i++) {
            acc = row_sub(acc,
                          load_row(psq_weights + (size_t) old_added[i] * NNUE_HALF_DIMENSIONS + d));
        }

        for (size_t i = 0; i < thr_removed_len; i++) {
            acc = row_sub(
              acc, load_row(thr_weights + (size_t) thr_removed[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < thr_added_len; i++) {
            acc = row_add(acc,
                          load_row(thr_weights + (size_t) thr_added[i] * NNUE_HALF_DIMENSIONS + d));
        }
        row_store((uint16_t *) (target + d), acc);
    }
}

void nnue_acc_accumulate_rows_i8(int16_t *target,
                                 const uint32_t *rows,
                                 size_t row_count,
                                 const int8_t *weights) {
    acc_rows_i8(true, target, rows, row_count, weights);
}

// Port upstream's `apply_combined` (nnue_accumulator.cpp): ONE combined accumulator (PSQ +
// Threats), loaded per tile once into a register, with both feature sets' removed/added weight
// rows applied in-register, then stored once. Integer add/sub commute under two's-complement
// wrap, so the final tile value equals
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
                                   const int8_t *psq_weights,
                                   const int8_t *thr_weights) {
    // Peel the PSQ lists' first row and make the second conditional, as upstream's
    // apply_psq_features<sign, Incremental> does. Both callers of this function are the
    // incremental step, and psq_changed_indices routes at most four diff sites into the
    // two lists -- `from` and `remove_sq` one way, `to` and `add_sq` the other -- so each
    // list holds one row or two, never none and never three. A count the compiler cannot
    // see costs a test and a taken branch per tile for a trip count that is always one or
    // two; peeled, the first row's load issues with no control dependency ahead of it.
    assert(psq_removed_len == 1 || psq_removed_len == 2);
    assert(psq_added_len == 1 || psq_added_len == 2);

    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        RowVecU16 acc = row_load((const uint16_t *) (source + d));
        acc =
          row_sub(acc, load_row(psq_weights + (size_t) psq_removed[0] * NNUE_HALF_DIMENSIONS + d));
        if (psq_removed_len > 1) {
            acc = row_sub(
              acc, load_row(psq_weights + (size_t) psq_removed[1] * NNUE_HALF_DIMENSIONS + d));
        }
        acc =
          row_add(acc, load_row(psq_weights + (size_t) psq_added[0] * NNUE_HALF_DIMENSIONS + d));
        if (psq_added_len > 1) {
            acc = row_add(acc,
                          load_row(psq_weights + (size_t) psq_added[1] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < thr_removed_len; i++) {
            acc = row_sub(
              acc, load_row(thr_weights + (size_t) thr_removed[i] * NNUE_HALF_DIMENSIONS + d));
        }
        for (size_t i = 0; i < thr_added_len; i++) {
            acc = row_add(acc,
                          load_row(thr_weights + (size_t) thr_added[i] * NNUE_HALF_DIMENSIONS + d));
        }
        row_store((uint16_t *) (target + d), acc);
    }
}
#else

// Hold the int8 weight rows one kernel call adds or removes, resolved from feature
// indices to row pointers ONCE, outside the tile loop -- upstream's WeightRows. Rows of
// both feature sets go in one list when they share a sign, so that nearly every row
// has a partner to be widened with (see tile_apply).
typedef struct {
    const int8_t *row[NNUE_PSQ_INDEX_CAPACITY + NNUE_THREAT_INDEX_CAPACITY];
    size_t count;
} RowList;

// Start an empty list. Only the count: a `{ .count = 0 }` initializer zero-fills the
// whole pointer array, which is over glibc's rep-stosb threshold and is invisible to
// the instruction counter while costing cycles on every call.
__attribute__((always_inline)) static inline RowList rows_empty(void) {
    RowList list;
    list.count = 0;
    return list;
}

static inline void
rows_append(RowList *list, const uint32_t *indices, size_t count, const int8_t *weights) {
    assert(list->count + count <= sizeof list->row / sizeof list->row[0]);
    for (size_t i = 0; i < count; i++)
        list->row[list->count++] = weights + (size_t) indices[i] * NNUE_HALF_DIMENSIONS;
}

// The tile is upstream SIMDTiling's eight registers of int16 lanes, and an int8 row's
// slice of it is four. Every helper is pinned inline: the tile is passed by value, and
// one left out of line would carry it through memory.
enum { TILE_REGS = ROW_TILE_WIDTH * sizeof(int16_t) / sizeof(__m128i) };
static_assert(TILE_REGS == 8, "the tile is upstream SIMDTiling's eight registers");

typedef struct {
    __m128i reg[TILE_REGS];
} Tile;

__attribute__((always_inline)) static inline Tile tile_load(const int16_t *p) {
    Tile t;
    for (size_t k = 0; k < TILE_REGS; k++)
        t.reg[k] = _mm_load_si128((const __m128i *) (p + 8 * k));
    return t;
}

__attribute__((always_inline)) static inline void tile_store(int16_t *p, Tile t) {
    for (size_t k = 0; k < TILE_REGS; k++)
        _mm_store_si128((__m128i *) (p + 8 * k), t.reg[k]);
}

__attribute__((always_inline)) static inline Tile tile_add_i16(Tile t, const int16_t *p, bool add) {
    for (size_t k = 0; k < TILE_REGS; k++) {
        const __m128i v = _mm_load_si128((const __m128i *) (p + 8 * k));
        t.reg[k] = add ? _mm_add_epi16(t.reg[k], v) : _mm_sub_epi16(t.reg[k], v);
    }
    return t;
}

// Widen two rows with one shuffle per register: interleave their bytes, then let
// pmaddubsw sum each (row 0, row 1) byte pair against a vector of ones. The sum of two
// int8 values cannot saturate int16, and int16 addition wraps the same in any order, so
// acc + (w0 + w1) is the value (acc + w0) + w1 is.
__attribute__((always_inline)) static inline Tile
tile_pair(Tile t, const int8_t *w0, const int8_t *w1, bool add) {
    const __m128i ones = _mm_set1_epi8(1);
    for (size_t k = 0; k < TILE_REGS / 2; k++) {
        const __m128i a = _mm_load_si128((const __m128i *) (w0 + 16 * k));
        const __m128i b = _mm_load_si128((const __m128i *) (w1 + 16 * k));
        const __m128i lo = _mm_maddubs_epi16(ones, _mm_unpacklo_epi8(a, b));
        const __m128i hi = _mm_maddubs_epi16(ones, _mm_unpackhi_epi8(a, b));
        t.reg[2 * k] = add ? _mm_add_epi16(t.reg[2 * k], lo) : _mm_sub_epi16(t.reg[2 * k], lo);
        t.reg[2 * k + 1] =
          add ? _mm_add_epi16(t.reg[2 * k + 1], hi) : _mm_sub_epi16(t.reg[2 * k + 1], hi);
    }
    return t;
}

// Widen one row, a list's odd one out: unpack each byte into the high half of its lane
// and shift it back down arithmetically, the widen the portable kernels lower to here.
__attribute__((always_inline)) static inline Tile tile_single(Tile t, const int8_t *w, bool add) {
    const __m128i zero = _mm_setzero_si128();
    for (size_t k = 0; k < TILE_REGS / 2; k++) {
        const __m128i a = _mm_load_si128((const __m128i *) (w + 16 * k));
        const __m128i lo = _mm_srai_epi16(_mm_unpacklo_epi8(zero, a), 8);
        const __m128i hi = _mm_srai_epi16(_mm_unpackhi_epi8(zero, a), 8);
        t.reg[2 * k] = add ? _mm_add_epi16(t.reg[2 * k], lo) : _mm_sub_epi16(t.reg[2 * k], lo);
        t.reg[2 * k + 1] =
          add ? _mm_add_epi16(t.reg[2 * k + 1], hi) : _mm_sub_epi16(t.reg[2 * k + 1], hi);
    }
    return t;
}

// Apply LIST to the tile at lane offset D, two rows at a time.
__attribute__((always_inline)) static inline Tile
tile_apply(Tile t, const RowList *list, size_t d, bool add) {
    size_t i = 0;
    for (; i + 1 < list->count; i += 2)
        t = tile_pair(t, list->row[i] + d, list->row[i + 1] + d, add);
    if (i < list->count)
        t = tile_single(t, list->row[i] + d, add);
    return t;
}

// Apply removed and added in ONE tile sweep -- upstream's apply_combined shape: hold the tile
// in registers, subtract every removed row and add every added row into it, then store the
// refreshed tile to BOTH cache_dest (in place) and state_dest before moving on, so the value
// reaches the ply's state slot as a register store rather than a trailing 2KB memcpy.
// Bit-identical however grouped: uint16 wrap addition is associative and commutative.
void nnue_acc_apply_delta_dual(int16_t *cache_dest,
                               int16_t *state_dest,
                               const uint32_t *removed,
                               size_t removed_len,
                               const uint32_t *added,
                               size_t added_len,
                               const int8_t *weights) {
    RowList removed_rows = rows_empty();
    RowList added_rows = rows_empty();
    rows_append(&removed_rows, removed, removed_len, weights);
    rows_append(&added_rows, added, added_len, weights);

    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        Tile acc = tile_load(cache_dest + d);
        acc = tile_apply(acc, &removed_rows, d, false);
        acc = tile_apply(acc, &added_rows, d, true);
        tile_store(cache_dest + d, acc);
        tile_store(state_dest + d, acc);
    }
}

void nnue_acc_apply_hybrid_delta(int16_t *target,
                                 const int16_t *source,
                                 int16_t *new_entry,
                                 const int16_t *old_entry,
                                 const uint32_t *new_removed,
                                 size_t new_removed_len,
                                 const uint32_t *new_added,
                                 size_t new_added_len,
                                 const uint32_t *old_removed,
                                 size_t old_removed_len,
                                 const uint32_t *old_added,
                                 size_t old_added_len,
                                 const uint32_t *thr_removed,
                                 size_t thr_removed_len,
                                 const uint32_t *thr_added,
                                 size_t thr_added_len,
                                 const int8_t *psq_weights,
                                 const int8_t *thr_weights) {
    RowList new_removed_rows = rows_empty();
    RowList new_added_rows = rows_empty();
    rows_append(&new_removed_rows, new_removed, new_removed_len, psq_weights);
    rows_append(&new_added_rows, new_added, new_added_len, psq_weights);

    // The source entry's removed rows are added back and its added rows removed, so
    // they share a list with the threat rows of the same sign.
    RowList removed_rows = rows_empty();
    RowList added_rows = rows_empty();
    rows_append(&added_rows, old_removed, old_removed_len, psq_weights);
    rows_append(&added_rows, thr_added, thr_added_len, thr_weights);
    rows_append(&removed_rows, old_added, old_added_len, psq_weights);
    rows_append(&removed_rows, thr_removed, thr_removed_len, thr_weights);

    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        Tile acc = tile_load(new_entry + d);
        acc = tile_apply(acc, &new_removed_rows, d, false);
        acc = tile_apply(acc, &new_added_rows, d, true);
        // The destination king's cache entry now describes the live board: store it
        // before the tile picks up terms that belong to this ply alone.
        tile_store(new_entry + d, acc);

        // The previous ply's combined accumulation carries the threat/pair rows this
        // move mostly keeps, plus PSQ rows bucketed on the SOURCE king square.
        // Subtracting that king's cache entry and re-applying its own diff removes
        // exactly those, leaving the destination bucket's rows already added above.
        acc = tile_add_i16(acc, source + d, true);
        acc = tile_add_i16(acc, old_entry + d, false);
        acc = tile_apply(acc, &removed_rows, d, false);
        acc = tile_apply(acc, &added_rows, d, true);
        tile_store(target + d, acc);
    }
}

void nnue_acc_accumulate_rows_i8(int16_t *target,
                                 const uint32_t *rows,
                                 size_t row_count,
                                 const int8_t *weights) {
    RowList list = rows_empty();
    rows_append(&list, rows, row_count, weights);

    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH)
        tile_store(target + d, tile_apply(tile_load(target + d), &list, d, true));
}

// Port upstream's `apply_combined` (nnue_accumulator.cpp): ONE combined accumulator (PSQ +
// Threats), loaded per tile once into registers, with both feature sets' removed/added
// weight rows applied in-register, then stored once. Integer add/sub commute under
// two's-complement wrap, so the final tile value equals
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
                                   const int8_t *psq_weights,
                                   const int8_t *thr_weights) {
    RowList removed_rows = rows_empty();
    RowList added_rows = rows_empty();
    rows_append(&removed_rows, psq_removed, psq_removed_len, psq_weights);
    rows_append(&removed_rows, thr_removed, thr_removed_len, thr_weights);
    rows_append(&added_rows, psq_added, psq_added_len, psq_weights);
    rows_append(&added_rows, thr_added, thr_added_len, thr_weights);

    for (size_t d = 0; d < NNUE_HALF_DIMENSIONS; d += ROW_TILE_WIDTH) {
        Tile acc = tile_load(source + d);
        acc = tile_apply(acc, &removed_rows, d, false);
        acc = tile_apply(acc, &added_rows, d, true);
        tile_store(target + d, acc);
    }
}
#endif  // ROW_PAIRED_WIDENING
