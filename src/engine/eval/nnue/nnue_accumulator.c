// Implement the incremental NNUE accumulator: the arena layout, the weight-row add/sub
// kernels, the refresh cache, the refresh/incremental update algorithm, and the
// transform. See nnue_accumulator.h for the incremental invariant.

#include "nnue_accumulator.h"

#include <assert.h>
#include <string.h>

#include "nnue_acc_rowops.h"
#include "simd.h"

// ---------------------------------------------------------------------------
// Arena layout
// ---------------------------------------------------------------------------

// Name the two feature sets by their storage slot. The single COMBINED accumulator
// (HalfKA + Threats) lives in the psq slot; the threat slot carries only that feature
// set's per-ply diff. This is upstream's apply_combined shape, not two accumulators.
enum { PSQ_FEATURE = 0, THREAT_FEATURE = 1 };

// Count which way up to the top slot was taken. See nnue_accumulator.h for why this
// is a gate rather than a debugging aid, and why it is absent from the release build.
#ifdef MCFISH_ACC_STATS
static NnueAccStats AccStats;
    #define ACC_STAT_BUMP(field) (void) (AccStats.field++)
const NnueAccStats *nnue_acc_stats(void) { return &AccStats; }
void nnue_acc_stats_reset(void) { AccStats = (NnueAccStats) { 0 }; }
#else
    #define ACC_STAT_BUMP(field) ((void) 0)
#endif

enum : size_t {
    ACC_BYTES = NNUE_COLOR_COUNT * NNUE_HALF_DIMENSIONS * sizeof(int16_t)
              + NNUE_COLOR_COUNT * NNUE_PSQT_BUCKETS * sizeof(int32_t)
              + NNUE_COLOR_COUNT * sizeof(bool),
    COMPUTED_OFFSET = NNUE_COLOR_COUNT * NNUE_HALF_DIMENSIONS * sizeof(int16_t)
                    + NNUE_COLOR_COUNT * NNUE_PSQT_BUCKETS * sizeof(int32_t),
    PSQT_OFFSET = NNUE_COLOR_COUNT * NNUE_HALF_DIMENSIONS * sizeof(int16_t),

    ACC_STATE_BYTES = NNUE_ROUND_UP(ACC_BYTES, NNUE_ALIGN),
    PSQ_DIFF_OFFSET = ACC_BYTES,
    // The threat slot carries ONLY the per-ply DirtyThreats diff -- the combined
    // accumulation and psqt live in the psq slot, never here (see PSQ_FEATURE below). So
    // the diff sits at the slot's front: reserving an accumulator-sized prefix here would
    // leave ~4 KiB dead per slot and stride consecutive plies' threat diffs a whole page
    // apart, which the incremental replay walk pays for as data-cache misses at depth.
    THREAT_DIFF_OFFSET = 0,
    PSQ_STATE_STRIDE = ACC_STATE_BYTES,
    THREAT_STATE_STRIDE = NNUE_ROUND_UP(THREAT_DIFF_OFFSET + sizeof(NnueDirtyThreats), NNUE_ALIGN),
    PSQ_ARRAY_BYTES = PSQ_STATE_STRIDE * NNUE_MAX_STACK_SIZE,
    THREAT_ARRAY_OFFSET = PSQ_ARRAY_BYTES,
    THREAT_ARRAY_BYTES = THREAT_STATE_STRIDE * NNUE_MAX_STACK_SIZE,
    STACK_SIZE_OFFSET = THREAT_ARRAY_OFFSET + THREAT_ARRAY_BYTES,
    STACK_BYTES = STACK_SIZE_OFFSET + sizeof(size_t),
    // Derive the us/prev_ksq/ksq offset from the field itself. The pawn-pair diff sits
    // between the list and those scalars, so they no longer follow the list directly.
    THREAT_REFRESH_DIFF_OFFSET = THREAT_DIFF_OFFSET + offsetof(NnueDirtyThreats, us),
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
// The board zone's DirtyThreats and this view alias the SAME arena bytes: pos_do_move
// writes through one, apply_combined reads through the other. Pin every field offset, not
// just the size — a reordering that keeps the size would corrupt only the incremental
// path, which the refresh path then hides from a shallow test.
static_assert(sizeof(NnueDirtyThreats) == sizeof(DirtyThreats),
              "the two DirtyThreats views must be the same size");
static_assert(offsetof(NnueDirtyThreats, list) == offsetof(DirtyThreats, list_values),
              "the threat list must lead both views");
static_assert(offsetof(NnueDirtyThreats, list.size) == offsetof(DirtyThreats, list_size), "");
static_assert(offsetof(NnueDirtyThreats, pawn_pairs.before) == offsetof(DirtyThreats, pp_before),
              "");
static_assert(offsetof(NnueDirtyThreats, pawn_pairs.after) == offsetof(DirtyThreats, pp_after), "");
static_assert(offsetof(NnueDirtyThreats, us) == offsetof(DirtyThreats, us), "");

size_t nnue_accumulator_stack_bytes(void) { return STACK_BYTES; }

// --- refresh-cache layout ---------------------------------------------------------

enum : size_t {
    CACHE_ENTRY_PSQT_OFFSET = NNUE_HALF_DIMENSIONS * sizeof(int16_t),
    CACHE_ENTRY_PIECES_OFFSET = CACHE_ENTRY_PSQT_OFFSET + NNUE_PSQT_BUCKETS * sizeof(int32_t),
    // Cache the entry's occupancy bitboard beside the piece array — upstream's
    // entry.pieceBB (nnue_accumulator.cpp:554,573). The refresh diff derives its
    // removed list as `changedBB & entry.pieceBB` and its added list from the live
    // occupancy, so the byte-per-square "was a piece here" tests drop out of the
    // changed-square walk. The 8 bytes sit inside the 64-byte round-up, so the
    // entry size does not move.
    CACHE_ENTRY_PIECE_BB_OFFSET = CACHE_ENTRY_PIECES_OFFSET + SQUARE_NB * sizeof(uint8_t),
    CACHE_ENTRY_BYTES = NNUE_ROUND_UP(CACHE_ENTRY_PIECE_BB_OFFSET + sizeof(uint64_t), NNUE_ALIGN),
    CACHE_BYTES = SQUARE_NB * NNUE_COLOR_COUNT * CACHE_ENTRY_BYTES,
};
// Pin the size claim above: adding the bitboard must not grow the rounded entry.
static_assert(CACHE_ENTRY_BYTES
                == NNUE_ROUND_UP(CACHE_ENTRY_PIECES_OFFSET + SQUARE_NB * sizeof(uint8_t),
                                 NNUE_ALIGN),
              "the piece bitboard must fit the entry's existing round-up slack");

size_t nnue_refresh_cache_bytes(void) { return CACHE_BYTES; }

// ---------------------------------------------------------------------------
// Weight-row kernels
// ---------------------------------------------------------------------------

// Set the lane count for the transform's clipped-ReLU pass. As with the row tile, the avx512
// build sweeps to 128 (4 steps over the 512-wide half-output, one 32-group nnz movemask per
// step); the narrower tiers keep 64, where the wider tile spills too many vector registers.
// The tile's types and ops are reached through width-selected macros so both tiers share one
// loop body.
#if defined(__AVX512F__)
enum { TRANSFORM_VEC_WIDTH = 128 };
    #define TfI16 NnueV128i16
    #define TfU16 NnueV128u16
    #define TfU32 NnueV128u32
    #define TfU8 NnueV128u8
    #define TfGroups NnueV32u32
    #define tf_i16_splat nnue_v128i16_splat
    #define tf_i16_load nnue_v128i16_load
    #define tf_i16_max nnue_v128i16_max
    #define tf_i16_min nnue_v128i16_min
    #define tf_i16_to_u16 nnue_v128_i16_to_u16
    #define tf_u16_shl nnue_v128u16_shl
    #define tf_u16_to_u32 nnue_v128_u16_to_u32
    #define tf_u32_mul nnue_v128u32_mul
    #define tf_u32_shr nnue_v128u32_shr
    #define tf_u32_to_u16 nnue_v128_u32_to_u16
    #define tf_u16_to_u8 nnue_v128_u16_to_u8
    #define tf_u8_store nnue_v128u8_store
    #define tf_u8_as_groups nnue_v128_u8_as_u32x32
    #define tf_movemask nnue_v32u32_movemask
#else
enum { TRANSFORM_VEC_WIDTH = 64 };
    #define TfI16 NnueV64i16
    #define TfU16 NnueV64u16
    #define TfU32 NnueV64u32
    #define TfU8 NnueV64u8
    #define TfGroups NnueV16u32
    #define tf_i16_splat nnue_v64i16_splat
    #define tf_i16_load nnue_v64i16_load
    #define tf_i16_max nnue_v64i16_max
    #define tf_i16_min nnue_v64i16_min
    #define tf_i16_to_u16 nnue_v64_i16_to_u16
    #define tf_u16_shl nnue_v64u16_shl
    #define tf_u16_to_u32 nnue_v64_u16_to_u32
    #define tf_u32_mul nnue_v64u32_mul
    #define tf_u32_shr nnue_v64u32_shr
    #define tf_u32_to_u16 nnue_v64_u32_to_u16
    #define tf_u16_to_u8 nnue_v64_u16_to_u8
    #define tf_u8_store nnue_v64u8_store
    #define tf_u8_as_groups nnue_v64_u8_as_u32x16
    #define tf_movemask nnue_v16u32_movemask
#endif
// Select the product step's native width — the register width whose vpackuswb the step
// narrows with. Upstream reaches its packus body from the generic `#else` arm, which
// every x86 tier takes (feature_transformer.h); only NEON, LSX/LASX and wasm branch
// away. Spell the choice once: the operand clamps, the step body and the nnz harvest
// all hang off it, and three independently written guards had already drifted into
// leaving the 512-bit tiers on the portable widening path alone.
//
// Gate the 512-bit arm on AVX512BW, the feature that owns vpmulhw and vpackuswb at zmm
// width — not on AVX512F, which has neither.
#if MCFISH_SIMD_VECTOR && defined(__AVX512BW__)
    #define TRANSFORM_PACKUS_BITS 512
#elif MCFISH_SIMD_VECTOR && defined(__AVX2__) && !defined(__AVX512F__)
    #define TRANSFORM_PACKUS_BITS 256
#elif MCFISH_SIMD_VECTOR && defined(__SSE2__) && !defined(__AVX512F__)
    #define TRANSFORM_PACKUS_BITS 128
#else
    #define TRANSFORM_PACKUS_BITS 0
#endif
static_assert((NNUE_HALF_DIMENSIONS / 2) % TRANSFORM_VEC_WIDTH == 0,
              "the transform half-output must be a multiple of TRANSFORM_VEC_WIDTH");
// The transform stores each step's nnz mask bytes exactly once at their own offset,
// so each perspective's chunk-bit span must fill whole words — no step may straddle
// a word another perspective seeds.
static_assert((NNUE_HALF_DIMENSIONS / 2 / 4) % 64 == 0,
              "each transform half must cover whole 64-bit nnz words");

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

// Diff a cache entry's cached board against BOARD/OCCUPANCY and split the changed
// squares into the HalfKA index lists that turn the entry's accumulation into the one
// BOARD wants, bucketed on KING_SQUARE.
//
// The finny refresh calls this with the live board; the hybrid king-move step calls it
// twice more, once per king square, and the second board it passes is a reconstruction
// of the position BEFORE the move rather than pos->board — which is why the occupancy
// is a parameter and not read from POS here.
static void entry_diff_indices(uint8_t perspective,
                               uint8_t king_square,
                               const uint8_t *entry_pieces,
                               uint64_t entry_piece_bb,
                               const uint8_t *board,
                               uint64_t occupancy,
                               uint32_t *removed,
                               size_t *removed_len_out,
                               uint32_t *added,
                               size_t *added_len_out) {
    size_t removed_len = 0;
    size_t added_len = 0;

#if MCFISH_SIMD_VECTOR && defined(__SSE2__)
    // Diff the cached board against the live one as a bitboard: explicit byte compares
    // collapse the 64 piece bytes into one changed-square mask, and the entry's cached
    // occupancy splits it into upstream's removedBB = changed & entry.pieceBB and
    // addedBB = changed & occupancy (nnue_accumulator.cpp:554,573) — so each list's
    // walk appends unconditionally, with no per-square piece test. Both walks pop
    // ascending, keeping the per-square ascending order of the chunked form. The
    // compares are spelled as intrinsics (pcmpeqb+pmovmskb / vpcmpeqb+vpmovmskb /
    // vpcmpb) because clang lowers the portable vector-compare form of this scan
    // through a generic reduce that LOSES to the chunked scalar diff at sse41.
    uint64_t changed;
    #if defined(__AVX512BW__)
    changed = _cvtmask64_u64(_mm512_cmpneq_epi8_mask(
      _mm512_loadu_si512((const void *) entry_pieces), _mm512_loadu_si512((const void *) board)));
    #elif defined(__AVX2__)
    const __m256i eq_lo = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *) entry_pieces),
                                            _mm256_loadu_si256((const __m256i *) board));
    const __m256i eq_hi =
      _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *) (entry_pieces + 32)),
                        _mm256_loadu_si256((const __m256i *) (board + 32)));
    changed = ~((uint64_t) (uint32_t) _mm256_movemask_epi8(eq_lo)
                | ((uint64_t) (uint32_t) _mm256_movemask_epi8(eq_hi) << 32));
    #else
    uint64_t equal = 0;
    for (unsigned quarter = 0; quarter < 4; quarter++) {
        const __m128i eq =
          _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *) (entry_pieces + quarter * 16)),
                         _mm_loadu_si128((const __m128i *) (board + quarter * 16)));
        equal |= (uint64_t) (uint32_t) _mm_movemask_epi8(eq) << (quarter * 16);
    }
    changed = ~equal;
    #endif

    uint64_t removed_bb = changed & entry_piece_bb;
    uint64_t added_bb = changed & occupancy;
    while (removed_bb != 0) {
        const unsigned square = (unsigned) __builtin_ctzll(removed_bb);
        removed_bb &= removed_bb - 1;
        removed[removed_len++] =
          nnue_half_make_index(perspective, (uint8_t) square, entry_pieces[square], king_square);
    }
    while (added_bb != 0) {
        const unsigned square = (unsigned) __builtin_ctzll(added_bb);
        added_bb &= added_bb - 1;
        added[added_len++] =
          nnue_half_make_index(perspective, (uint8_t) square, board[square], king_square);
    }
#else
    // Diff the cached board against the live one eight squares at a time: XOR an 8-byte
    // chunk of each and skip the equal chunks, walking only the differing bytes. The
    // per-square scan-and-append loop runs all 64 squares scalar (the appends block the
    // vectorizer), where most chunks compare equal in one branch here. Bytes are walked
    // low to high, so both lists keep the per-square ascending order of the fused form.
    for (unsigned chunk = 0; chunk < SQUARE_NB / 8; chunk++) {
        uint64_t old_eight;
        uint64_t new_eight;
        memcpy(&old_eight, entry_pieces + chunk * 8, sizeof old_eight);
        memcpy(&new_eight, board + chunk * 8, sizeof new_eight);
        uint64_t diff = old_eight ^ new_eight;
        while (diff != 0) {
            const unsigned byte = (unsigned) __builtin_ctzll(diff) >> 3;
            diff &= ~(0xffULL << (byte * 8));
            const unsigned square = chunk * 8 + byte;
            const uint8_t old_piece = (uint8_t) (old_eight >> (byte * 8));
            const uint8_t new_piece = (uint8_t) (new_eight >> (byte * 8));
            if (old_piece != NO_PIECE)
                removed[removed_len++] =
                  nnue_half_make_index(perspective, (uint8_t) square, old_piece, king_square);
            if (new_piece != NO_PIECE)
                added[added_len++] =
                  nnue_half_make_index(perspective, (uint8_t) square, new_piece, king_square);
        }
    }
#endif

    *removed_len_out = removed_len;
    *added_len_out = added_len;
}

// Refresh the HalfKA half through the finny table: diff the cached board against the
// live one, apply that delta to the cached accumulation, then copy it into the slot.
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
    size_t removed_len;
    size_t added_len;

    uint64_t entry_piece_bb;
    memcpy(&entry_piece_bb, entry + CACHE_ENTRY_PIECE_BB_OFFSET, sizeof entry_piece_bb);
    entry_diff_indices(perspective, king_square, entry_pieces, entry_piece_bb, board,
                       pos->by_type[ALL_PIECES], removed, &removed_len, added, &added_len);

    // Dual-store: write the refreshed row into BOTH the cache entry (in place, for next time)
    // and this ply's state slot in one tiled pass, so the cache→state copy is a register store
    // rather than a trailing memcpy of the accumulation and psqt rows.
    nnue_acc_apply_delta_i16_dual(
      cache_entry_accumulation(entry),
      state_accumulation_mut(PSQ_FEATURE, latest_index, stack, perspective), removed, removed_len,
      added, added_len, nnue_ft_psq_weights(ft));
    nnue_acc_apply_psqt_delta_dual(
      cache_entry_psqt(entry), state_psqt_mut(PSQ_FEATURE, latest_index, stack, perspective),
      removed, removed_len, added, added_len, nnue_ft_psq_psqt_weights(ft));

    memcpy(entry_pieces, board, SQUARE_NB);
    // Keep the cached occupancy in step with the piece array: the next refresh's
    // removed split reads it. The scalar build's chunked diff never reads it, but the
    // store keeps every build's cache state identical.
    memcpy(entry + CACHE_ENTRY_PIECE_BB_OFFSET, &pos->by_type[ALL_PIECES], sizeof(uint64_t));

    state_bytes_mut(PSQ_FEATURE, latest_index, stack)[COMPUTED_OFFSET + perspective] = 1;
}

// Perform the fused refresh: the HalfKA half through the finny cache fills the combined
// accumulation and sets computed; the threat features are then ADDED on top (additive, no
// zeroing), so combined = psq + threat — the refresh half of apply_combined.
//
// Keep this OUT of the caller's body. Only a king move needs a refresh, so this path runs
// a small fraction of the evaluations the incremental step does, but it carries the two
// widest kernels in the file — the board diff plus both dual-store tile sweeps, plus the
// full active-threat accumulate — and LTO inlines all of it into the one function that
// every evaluation enters. That is what makes the transform the largest hot object in the
// binary: with the refresh folded in it does not fit the instruction cache at all, and the
// cold half is fetched alongside the hot half on every pass. Outlining it also stops the
// body being duplicated at each inline site, so it costs no instructions to move — it
// saves them. Attach the attribute rather than trusting an inline-cost heuristic, which
// has no notion of how rarely a king moves.
__attribute__((noinline)) static void refresh_combined(uint8_t perspective,
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
    // Append the pawn-pair rows onto the SAME list: they index the shared threat/pair
    // weight region, so one accumulate pass below covers both feature sets.
    nnue_pair_append_active(&active, perspective, king_square,
                            pos->by_color[WHITE] & pos->by_type[PAWN],
                            pos->by_color[BLACK] & pos->by_type[PAWN]);

    nnue_acc_accumulate_rows_i8(
      state_accumulation_mut(PSQ_FEATURE, latest_index, stack, perspective), active.indices,
      active.len, nnue_ft_threat_weights(ft));
    nnue_acc_accumulate_psqt_rows(state_psqt_mut(PSQ_FEATURE, latest_index, stack, perspective),
                                  active.indices, active.len, nnue_ft_threat_psqt_weights(ft));
}

// Build the threat AND pawn-pair changed-feature lists for one ply's diff, bucketed on
// KING_SQUARE. Both sets append to the same pair of lists, because both index the
// shared threat/pair weight region.
//
// FORWARD says which direction the step runs: going backward every role inverts, which
// is folded into the walk mask for the threats and expressed by swapping the two lists
// for the pairs, exactly as upstream swaps its arguments.
static void threat_changed_indices(uint8_t perspective,
                                   uint8_t king_square,
                                   const NnueDirtyThreats *thr_diff,
                                   bool forward,
                                   const int8_t *thr_weights,
                                   uint32_t *removed,
                                   size_t *removed_len_out,
                                   uint32_t *added,
                                   size_t *added_len_out) {
    size_t removed_len = 0;
    size_t added_len = 0;

    const NnueDirtyThreatRaw *thr_list = thr_diff->list.values;
    const size_t thr_list_len = thr_diff->list.size;

    // One walk over the dirty threats: orient the whole record with ONE xor against the
    // per-walk mask (nnue_full_orient_mask broadcasts the orientation and swap onto the
    // record's own lanes and folds the walk direction into bit 31), compute the feature
    // index from the already-oriented fields, preload its scattered weight row before the
    // apply reads it (read hint, low locality), and route by the xored record's sign
    // alone -- upstream append_changed_indices' `insert = add ? added : removed`. An
    // excluded pair indexes past NNUE_FULL_DIMENSIONS, which is the exclusion mechanism
    // (see nnue_feature.h). Keep the index math scalar: under LTO clang auto-vectorizes
    // this loop into vpgatherqd table lookups (IndexLut1/Offsets/IndexLut2), which cost
    // far more on Zen4 than the scalar loads upstream emits; the trip count is tiny so
    // the vector form has nothing to recover.
    const uint32_t walk_mask = nnue_full_orient_mask(perspective, king_square, forward);
#pragma clang loop vectorize(disable) interleave(disable)
    for (size_t list_index = 0; list_index < thr_list_len; list_index++) {
        const uint32_t rec = thr_list[list_index].data ^ walk_mask;
        const uint8_t attacker_o = (uint8_t) ((rec >> NNUE_DIRTY_THREAT_PC_SHIFT) & 0xf);
        const uint8_t attacked_o = (uint8_t) ((rec >> NNUE_DIRTY_THREATENED_PC_SHIFT) & 0xf);
        const uint8_t to_o = (uint8_t) ((rec >> NNUE_DIRTY_THREATENED_SQ_SHIFT) & 0xff);
        const uint8_t from_o = (uint8_t) ((rec >> NNUE_DIRTY_THREAT_PC_SQ_SHIFT) & 0xff);
        const uint32_t index = nnue_full_make_index_oriented(attacker_o, from_o, to_o, attacked_o);
        __builtin_prefetch(thr_weights + (size_t) index * NNUE_HALF_DIMENSIONS, 0, 1);
        if (index >= NNUE_FULL_DIMENSIONS) {
            continue;
        }
        if ((int32_t) rec < 0) {
            added[added_len++] = index;
        } else {
            removed[removed_len++] = index;
        }
    }

    // Append the pawn-pair delta onto the SAME lists -- pair indices address the tail of
    // the shared threat/pair weight region. The producer puts appearing pairs in its
    // `added` argument and disappearing ones in `removed`; a backward walk inverts every
    // role, so the two lists are passed swapped.
    if (forward) {
        nnue_pair_append_changed(perspective, king_square, &thr_diff->pawn_pairs, removed,
                                 &removed_len, added, &added_len);
    } else {
        nnue_pair_append_changed(perspective, king_square, &thr_diff->pawn_pairs, added, &added_len,
                                 removed, &removed_len);
    }

    *removed_len_out = removed_len;
    *added_len_out = added_len;
}

// Build both perspectives' threat and pawn-pair changed-feature lists from ONE walk of
// the ply's diff. The dirty records and the pawn geometry are perspective-independent;
// only the orientation the index is computed under differs, so the loads, the
// add/remove routing and the pair topology are paid once instead of twice. Upstream's
// FullThreats/PP_3Wide::append_changed_indices_both.
//
// Forward walks only: the shared step exists for the common suffix of a two-perspective
// forward catch-up, where every ply's diff belongs to its own target slot.
static void threat_changed_indices_both(uint8_t white_ksq,
                                        uint8_t black_ksq,
                                        const NnueDirtyThreats *thr_diff,
                                        const int8_t *thr_weights,
                                        uint32_t *white_removed,
                                        size_t *white_removed_len_out,
                                        uint32_t *white_added,
                                        size_t *white_added_len_out,
                                        uint32_t *black_removed,
                                        size_t *black_removed_len_out,
                                        uint32_t *black_added,
                                        size_t *black_added_len_out) {
    size_t white_removed_len = 0;
    size_t white_added_len = 0;
    size_t black_removed_len = 0;
    size_t black_added_len = 0;

    const NnueDirtyThreatRaw *thr_list = thr_diff->list.values;
    const size_t thr_list_len = thr_diff->list.size;
    const uint32_t white_mask = nnue_full_orient_mask(WHITE, white_ksq, true);
    const uint32_t black_mask = nnue_full_orient_mask(BLACK, black_ksq, true);

#pragma clang loop vectorize(disable) interleave(disable)
    for (size_t list_index = 0; list_index < thr_list_len; list_index++) {
        const uint32_t data = thr_list[list_index].data;
        // A forward mask leaves bit 31 alone, so the raw record's own sign is the
        // add/remove decision and it is the same for both perspectives.
        const bool add = (int32_t) data < 0;

        const uint32_t white_rec = data ^ white_mask;
        const uint32_t white_index = nnue_full_make_index_oriented(
          (uint8_t) ((white_rec >> NNUE_DIRTY_THREAT_PC_SHIFT) & 0xf),
          (uint8_t) ((white_rec >> NNUE_DIRTY_THREAT_PC_SQ_SHIFT) & 0xff),
          (uint8_t) ((white_rec >> NNUE_DIRTY_THREATENED_SQ_SHIFT) & 0xff),
          (uint8_t) ((white_rec >> NNUE_DIRTY_THREATENED_PC_SHIFT) & 0xf));
        const uint32_t black_rec = data ^ black_mask;
        const uint32_t black_index = nnue_full_make_index_oriented(
          (uint8_t) ((black_rec >> NNUE_DIRTY_THREAT_PC_SHIFT) & 0xf),
          (uint8_t) ((black_rec >> NNUE_DIRTY_THREAT_PC_SQ_SHIFT) & 0xff),
          (uint8_t) ((black_rec >> NNUE_DIRTY_THREATENED_SQ_SHIFT) & 0xff),
          (uint8_t) ((black_rec >> NNUE_DIRTY_THREATENED_PC_SHIFT) & 0xf));

        __builtin_prefetch(thr_weights + (size_t) white_index * NNUE_HALF_DIMENSIONS, 0, 1);
        __builtin_prefetch(thr_weights + (size_t) black_index * NNUE_HALF_DIMENSIONS, 0, 1);

        if (white_index < NNUE_FULL_DIMENSIONS) {
            if (add) {
                white_added[white_added_len++] = white_index;
            } else {
                white_removed[white_removed_len++] = white_index;
            }
        }
        if (black_index < NNUE_FULL_DIMENSIONS) {
            if (add) {
                black_added[black_added_len++] = black_index;
            } else {
                black_removed[black_removed_len++] = black_index;
            }
        }
    }

    nnue_pair_append_changed_both(white_ksq, black_ksq, &thr_diff->pawn_pairs, white_removed,
                                  &white_removed_len, white_added, &white_added_len, black_removed,
                                  &black_removed_len, black_added, &black_added_len);

    *white_removed_len_out = white_removed_len;
    *white_added_len_out = white_added_len;
    *black_removed_len_out = black_removed_len;
    *black_added_len_out = black_added_len;
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

// Route each HalfKA index at its diff site, upstream append_changed_indices' shape
// (half_ka_v2_hm.cpp): every diff condition is tested once and the computed index goes
// straight into its list -- no intermediate buffer, no second routing pass. Same
// conditions in the same order as the buffered form, so both lists keep their per-list
// order.
static void psq_changed_indices(uint8_t perspective,
                                uint8_t king_square,
                                NnueDirtyPiece psq_diff,
                                bool forward,
                                uint32_t *removed,
                                size_t *removed_len_out,
                                uint32_t *added,
                                size_t *added_len_out) {
    size_t removed_len = 0;
    size_t added_len = 0;

    append_half_change(removed, &removed_len, added, &added_len,
                       nnue_half_make_index(perspective, psq_diff.from, psq_diff.pc, king_square),
                       forward);
    if (psq_diff.to != NNUE_SQ_NONE) {
        append_half_change(removed, &removed_len, added, &added_len,
                           nnue_half_make_index(perspective, psq_diff.to, psq_diff.pc, king_square),
                           !forward);
    }
    if (psq_diff.remove_sq != NNUE_SQ_NONE) {
        append_half_change(
          removed, &removed_len, added, &added_len,
          nnue_half_make_index(perspective, psq_diff.remove_sq, psq_diff.remove_pc, king_square),
          forward);
    }
    if (psq_diff.add_sq != NNUE_SQ_NONE) {
        append_half_change(
          removed, &removed_len, added, &added_len,
          nnue_half_make_index(perspective, psq_diff.add_sq, psq_diff.add_pc, king_square),
          !forward);
    }

    *removed_len_out = removed_len;
    *added_len_out = added_len;
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

    uint32_t psq_removed[NNUE_PSQ_INDEX_CAPACITY];
    uint32_t psq_added[NNUE_PSQ_INDEX_CAPACITY];
    size_t psq_removed_len;
    size_t psq_added_len;
    psq_changed_indices(perspective, king_square, psq_diff, forward, psq_removed, &psq_removed_len,
                        psq_added, &psq_added_len);

    const NnueDirtyThreats *thr_diff = threat_diff_at(
      state_bytes_const(THREAT_FEATURE, forward ? target_index : computed_index, stack));

    uint32_t thr_removed[NNUE_THREAT_INDEX_CAPACITY];
    uint32_t thr_added[NNUE_THREAT_INDEX_CAPACITY];
    size_t thr_removed_len;
    size_t thr_added_len;

    threat_changed_indices(perspective, king_square, thr_diff, forward, nnue_ft_threat_weights(ft),
                           thr_removed, &thr_removed_len, thr_added, &thr_added_len);

    nnue_acc_apply_combined_delta(
      state_accumulation_mut(PSQ_FEATURE, target_index, stack, perspective),
      state_accumulation_const(PSQ_FEATURE, computed_index, stack, perspective), psq_removed,
      psq_removed_len, psq_added, psq_added_len, thr_removed, thr_removed_len, thr_added,
      thr_added_len, nnue_ft_psq_weights(ft), nnue_ft_threat_weights(ft));
    nnue_acc_apply_combined_psqt_delta(
      state_psqt_mut(PSQ_FEATURE, target_index, stack, perspective),
      state_psqt_const(PSQ_FEATURE, computed_index, stack, perspective), psq_removed,
      psq_removed_len, psq_added, psq_added_len, thr_removed, thr_removed_len, thr_added,
      thr_added_len, nnue_ft_psq_psqt_weights(ft), nnue_ft_threat_psqt_weights(ft));
    state_bytes_mut(PSQ_FEATURE, target_index, stack)[COMPUTED_OFFSET + perspective] = 1;
}

// Take one forward incremental step for BOTH perspectives — a port of upstream's
// update_accumulator_incremental_both. The ply's diff is read once, the threat and
// pawn-pair lists come from one shared walk, and only the HalfKA indices and the
// accumulator arithmetic stay per-perspective. Every list holds exactly what two
// separate steps would have produced, in the same order.
static void apply_combined_both(NnueAccumulatorStack *stack,
                                const NnueFeatureTransformer *ft,
                                uint8_t white_ksq,
                                uint8_t black_ksq,
                                size_t target_index,
                                size_t computed_index) {
    assert(state_computed(stack, PSQ_FEATURE, computed_index, WHITE));
    assert(state_computed(stack, PSQ_FEATURE, computed_index, BLACK));
    assert(!state_computed(stack, PSQ_FEATURE, target_index, WHITE));
    assert(!state_computed(stack, PSQ_FEATURE, target_index, BLACK));

    const NnueDirtyPiece psq_diff =
      psq_diff_at(state_bytes_const(PSQ_FEATURE, target_index, stack));

    uint32_t psq_removed[NNUE_COLOR_COUNT][NNUE_PSQ_INDEX_CAPACITY];
    uint32_t psq_added[NNUE_COLOR_COUNT][NNUE_PSQ_INDEX_CAPACITY];
    size_t psq_removed_len[NNUE_COLOR_COUNT];
    size_t psq_added_len[NNUE_COLOR_COUNT];
    psq_changed_indices(WHITE, white_ksq, psq_diff, true, psq_removed[WHITE],
                        &psq_removed_len[WHITE], psq_added[WHITE], &psq_added_len[WHITE]);
    psq_changed_indices(BLACK, black_ksq, psq_diff, true, psq_removed[BLACK],
                        &psq_removed_len[BLACK], psq_added[BLACK], &psq_added_len[BLACK]);

    uint32_t thr_removed[NNUE_COLOR_COUNT][NNUE_THREAT_INDEX_CAPACITY];
    uint32_t thr_added[NNUE_COLOR_COUNT][NNUE_THREAT_INDEX_CAPACITY];
    size_t thr_removed_len[NNUE_COLOR_COUNT];
    size_t thr_added_len[NNUE_COLOR_COUNT];
    threat_changed_indices_both(
      white_ksq, black_ksq, threat_diff_at(state_bytes_const(THREAT_FEATURE, target_index, stack)),
      nnue_ft_threat_weights(ft), thr_removed[WHITE], &thr_removed_len[WHITE], thr_added[WHITE],
      &thr_added_len[WHITE], thr_removed[BLACK], &thr_removed_len[BLACK], thr_added[BLACK],
      &thr_added_len[BLACK]);

    for (uint8_t perspective = 0; perspective < NNUE_COLOR_COUNT; perspective++) {
        nnue_acc_apply_combined_delta(
          state_accumulation_mut(PSQ_FEATURE, target_index, stack, perspective),
          state_accumulation_const(PSQ_FEATURE, computed_index, stack, perspective),
          psq_removed[perspective], psq_removed_len[perspective], psq_added[perspective],
          psq_added_len[perspective], thr_removed[perspective], thr_removed_len[perspective],
          thr_added[perspective], thr_added_len[perspective], nnue_ft_psq_weights(ft),
          nnue_ft_threat_weights(ft));
        nnue_acc_apply_combined_psqt_delta(
          state_psqt_mut(PSQ_FEATURE, target_index, stack, perspective),
          state_psqt_const(PSQ_FEATURE, computed_index, stack, perspective),
          psq_removed[perspective], psq_removed_len[perspective], psq_added[perspective],
          psq_added_len[perspective], thr_removed[perspective], thr_removed_len[perspective],
          thr_added[perspective], thr_added_len[perspective], nnue_ft_psq_psqt_weights(ft),
          nnue_ft_threat_psqt_weights(ft));
        state_bytes_mut(PSQ_FEATURE, target_index, stack)[COMPUTED_OFFSET + perspective] = 1;
    }
}

// Bound the hybrid king-move step by piece count, upstream's MIN_PC_COUNT_HYBRID
// (nnue_accumulator.cpp). Below it a position carries few enough threat/pair features
// that summing them from scratch beats reconstructing the source king's cached HalfKA
// accumulation.
enum { MIN_PC_COUNT_HYBRID = 15 };

// Take a king move incrementally — a port of upstream's update_accumulator_hybrid.
//
// A king move rebuckets every HalfKA index, so before this the whole accumulation had
// to be rebuilt from the refresh cache and the full active threat/pair set. But the
// threat and pair features only change orientation when the king crosses the centre
// FILE, so when it stays on its half their accumulation carries over from the previous
// ply and only this ply's own threat delta applies. What remains is to swap the HalfKA
// half of the accumulation from the source king's bucket to the destination's, which
// the two finny cache entries give:
//
//   target = computed - <source-bucket HalfKA> + <destination-bucket HalfKA>
//
// Both halves come from a cache entry plus that entry's diff against the board it
// should describe. The destination's is the live board; the source's is the position
// BEFORE the move, which is reconstructed here by undoing the move on a copy of the
// piece array -- the only board this file builds rather than reads.
static void update_accumulator_hybrid(uint8_t perspective,
                                      NnueAccumulatorStack *stack,
                                      const Position *pos,
                                      const NnueFeatureTransformer *ft,
                                      NnueRefreshCache *cache) {
    const size_t target_index = stack_size(stack) - 1;
    const size_t computed_index = target_index - 1;
    const NnueDirtyPiece diff = psq_diff_at(state_bytes_const(PSQ_FEATURE, target_index, stack));
    const uint8_t old_ksq = diff.from;
    const uint8_t new_ksq = diff.to;

    assert(state_computed(stack, PSQ_FEATURE, computed_index, perspective));
    assert(!state_computed(stack, PSQ_FEATURE, target_index, perspective));
    assert(old_ksq != new_ksq);

    // Undo the move on a copy of the piece array. A capture put the king on the
    // captured piece's square, so that square reverts to the captured piece and the
    // occupancy is unchanged; a quiet move empties it.
    uint8_t prev_board[SQUARE_NB];
    memcpy(prev_board, pos->board, SQUARE_NB);
    uint64_t prev_occupancy = pos->by_type[ALL_PIECES];
    if (diff.remove_sq != NNUE_SQ_NONE) {
        assert(diff.remove_sq == new_ksq);
        prev_board[new_ksq] = diff.remove_pc;
    } else {
        prev_board[new_ksq] = NO_PIECE;
        prev_occupancy &= ~(1ULL << new_ksq);
    }
    prev_board[old_ksq] = diff.pc;
    prev_occupancy |= 1ULL << old_ksq;

    unsigned char *const old_entry = cache_entry(cache, old_ksq, perspective);
    unsigned char *const new_entry = cache_entry(cache, new_ksq, perspective);
    uint64_t old_entry_bb;
    uint64_t new_entry_bb;
    memcpy(&old_entry_bb, old_entry + CACHE_ENTRY_PIECE_BB_OFFSET, sizeof old_entry_bb);
    memcpy(&new_entry_bb, new_entry + CACHE_ENTRY_PIECE_BB_OFFSET, sizeof new_entry_bb);

    // "removed"/"added" are relative to each ENTRY: the rows to take off and put on to
    // turn that entry's cached accumulation into the one its board wants.
    uint32_t old_removed[NNUE_PSQ_INDEX_CAPACITY];
    uint32_t old_added[NNUE_PSQ_INDEX_CAPACITY];
    uint32_t new_removed[NNUE_PSQ_INDEX_CAPACITY];
    uint32_t new_added[NNUE_PSQ_INDEX_CAPACITY];
    size_t old_removed_len;
    size_t old_added_len;
    size_t new_removed_len;
    size_t new_added_len;
    entry_diff_indices(perspective, old_ksq, cache_entry_pieces(old_entry), old_entry_bb,
                       prev_board, prev_occupancy, old_removed, &old_removed_len, old_added,
                       &old_added_len);
    entry_diff_indices(perspective, new_ksq, cache_entry_pieces(new_entry), new_entry_bb,
                       (const uint8_t *) pos->board, pos->by_type[ALL_PIECES], new_removed,
                       &new_removed_len, new_added, &new_added_len);

    uint32_t thr_removed[NNUE_THREAT_INDEX_CAPACITY];
    uint32_t thr_added[NNUE_THREAT_INDEX_CAPACITY];
    size_t thr_removed_len;
    size_t thr_added_len;
    threat_changed_indices(
      perspective, new_ksq, threat_diff_at(state_bytes_const(THREAT_FEATURE, target_index, stack)),
      true, nnue_ft_threat_weights(ft), thr_removed, &thr_removed_len, thr_added, &thr_added_len);

    nnue_acc_apply_hybrid_delta(
      state_accumulation_mut(PSQ_FEATURE, target_index, stack, perspective),
      state_accumulation_const(PSQ_FEATURE, computed_index, stack, perspective),
      cache_entry_accumulation(new_entry), cache_entry_accumulation(old_entry), new_removed,
      new_removed_len, new_added, new_added_len, old_removed, old_removed_len, old_added,
      old_added_len, thr_removed, thr_removed_len, thr_added, thr_added_len,
      nnue_ft_psq_weights(ft), nnue_ft_threat_weights(ft));
    nnue_acc_apply_hybrid_psqt_delta(
      state_psqt_mut(PSQ_FEATURE, target_index, stack, perspective),
      state_psqt_const(PSQ_FEATURE, computed_index, stack, perspective),
      cache_entry_psqt(new_entry), cache_entry_psqt(old_entry), new_removed, new_removed_len,
      new_added, new_added_len, old_removed, old_removed_len, old_added, old_added_len, thr_removed,
      thr_removed_len, thr_added, thr_added_len, nnue_ft_psq_psqt_weights(ft),
      nnue_ft_threat_psqt_weights(ft));

    // The destination entry was refreshed in place above, so its cached board must
    // follow. The source entry is untouched.
    memcpy(cache_entry_pieces(new_entry), pos->board, SQUARE_NB);
    memcpy(new_entry + CACHE_ENTRY_PIECE_BB_OFFSET, &pos->by_type[ALL_PIECES], sizeof(uint64_t));

    state_bytes_mut(PSQ_FEATURE, target_index, stack)[COMPUTED_OFFSET + perspective] = 1;
}

// Report whether the latest ply is a king move the hybrid step can take: PERSPECTIVE's
// own king, landing on the same half of the board (so the threat/pair orientation is
// unchanged), with the previous ply computed and enough pieces on the board to pay for
// the two cache diffs. Castling is excluded for simplicity, as upstream does -- it is
// the one king move that also relocates a rook, which `add_sq` records.
static bool
hybrid_applicable(uint8_t perspective, const NnueAccumulatorStack *stack, const Position *pos) {
    const size_t size = stack_size(stack);
    if (size < 2)
        return false;

    const NnueDirtyPiece diff = psq_diff_at(state_bytes_const(PSQ_FEATURE, size - 1, stack));
    return diff.pc == (uint8_t) (6 + 8 * perspective)
        && state_computed(stack, PSQ_FEATURE, size - 2, perspective)
        && __builtin_popcountll(pos->by_type[ALL_PIECES]) >= MIN_PC_COUNT_HYBRID
        && ((diff.from & 0x4) == (diff.to & 0x4)) && diff.add_sq == NNUE_SQ_NONE;
}

// Walk the stack once for PERSPECTIVE over the combined accumulator — a port of upstream's
// AccumulatorStack::evaluate_side. find_last_usable consults ONLY the HalfKA refresh
// condition, because a threat refresh (a king move across the centre) is a subset of a
// HalfKA refresh, so the combined accumulator always refreshes together.
static void evaluate_side(uint8_t perspective,
                          NnueAccumulatorStack *stack,
                          const Position *pos,
                          const NnueFeatureTransformer *ft,
                          NnueRefreshCache *cache,
                          size_t last_usable) {
    const size_t size = stack_size(stack);
    const uint8_t king_square = nnue_king_square(pos, perspective);

    if (state_computed(stack, PSQ_FEATURE, last_usable, perspective)) {
        for (size_t next = last_usable + 1; next < size; next++) {
            apply_combined(stack, perspective, ft, king_square, next, next - 1, true);
        }
    } else if (hybrid_applicable(perspective, stack, pos)) {
        ACC_STAT_BUMP(hybrid);
        update_accumulator_hybrid(perspective, stack, pos, ft, cache);
    } else {
        ACC_STAT_BUMP(refresh);
        refresh_combined(perspective, king_square, stack, pos, ft, cache);

        for (size_t computed_index = size - 1; computed_index > last_usable; computed_index--) {
            apply_combined(stack, perspective, ft, king_square, computed_index - 1, computed_index,
                           false);
        }
    }
}

// Walk the stack forward for BOTH perspectives — a port of upstream's
// forward_update_incremental_both. The two perspectives can be usable from different
// plies, so first catch the lagging one up alone; from there the remaining plies are
// common to both and the shared step takes them, reading each ply's diff once.
static void forward_update_both(NnueAccumulatorStack *stack,
                                const Position *pos,
                                const NnueFeatureTransformer *ft,
                                size_t white_begin,
                                size_t black_begin) {
    const size_t size = stack_size(stack);
    const uint8_t white_ksq = nnue_king_square(pos, WHITE);
    const uint8_t black_ksq = nnue_king_square(pos, BLACK);
    const size_t shared_begin = white_begin > black_begin ? white_begin : black_begin;

    for (size_t next = white_begin + 1; next <= shared_begin; next++)
        apply_combined(stack, WHITE, ft, white_ksq, next, next - 1, true);
    for (size_t next = black_begin + 1; next <= shared_begin; next++)
        apply_combined(stack, BLACK, ft, black_ksq, next, next - 1, true);

    for (size_t next = shared_begin + 1; next < size; next++) {
        ACC_STAT_BUMP(shared_step);
        apply_combined_both(stack, ft, white_ksq, black_ksq, next, next - 1);
    }
}

void nnue_acc_evaluate(NnueAccumulatorStack *stack,
                       const Position *pos,
                       const NnueFeatureTransformer *ft,
                       NnueRefreshCache *cache) {
    // Match upstream AccumulatorStack::evaluate: one combined (HalfKA + Threats) pass per
    // perspective, not one per (feature, perspective). When NEITHER perspective needs a
    // refresh the whole update is a forward walk, and the two walks share their suffix.
    const size_t last_white = find_last_usable(PSQ_FEATURE, stack, WHITE);
    const size_t last_black = find_last_usable(PSQ_FEATURE, stack, BLACK);

    if (state_computed(stack, PSQ_FEATURE, last_white, WHITE)
        && state_computed(stack, PSQ_FEATURE, last_black, BLACK)) {
        ACC_STAT_BUMP(shared_walk);
        forward_update_both(stack, pos, ft, last_white, last_black);
    } else {
        ACC_STAT_BUMP(split_walk);
        evaluate_side(WHITE, stack, pos, ft, cache, last_white);
        evaluate_side(BLACK, stack, pos, ft, cache, last_black);
    }
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
#if TRANSFORM_PACKUS_BITS == 0
    const TfI16 zero = tf_i16_splat(0);
    const TfI16 c255 = tf_i16_splat(255);
#endif

#if !(MCFISH_SIMD_VECTOR && defined(__SSE2__)) && !defined(__AVX512F__)
    // Only the scalar fallback still seeds the bitset and ORs each mask into its
    // word: every x86 tier stores each step's mask bytes directly at their own
    // offset — upstream's `*out++` NNZCursor shape — so every bitset byte is
    // written exactly once and the zero-fill drops. (The earlier sse41 refutation
    // was the store-once u64 accumulator, a different shape: it carried the word
    // in a register across steps; the direct byte store carries nothing.)
    memset(nnz, 0, sizeof(NnueNnzBitset));
#endif

    for (size_t p = 0; p < 2; p++) {
        const size_t pp = p == 0 ? p0 : p1;
        const size_t offset = half * p;
        const size_t base = pp * NNUE_HALF_DIMENSIONS;
#if TRANSFORM_PACKUS_BITS >= 256
        // Rebase the bitset cursor per perspective so the store below indexes it
        // with the loop counter alone; folding the perspective offset into the
        // index expression instead makes clang carry a second, or-adjusted cursor
        // register through the loop.
        unsigned char *const nnz_bytes = (unsigned char *) *nnz + offset / 32;
#endif
        for (size_t j = 0; j < half; j += TRANSFORM_VEC_WIDTH) {
#if TRANSFORM_PACKUS_BITS == 512
            // Native avx512bw product step, upstream's packus-clip shape at zmm width.
            // Same identity the avx2 branch below documents: clamp the FIRST operand
            // from above and below, the second from above only — when the second stays
            // negative the SIGNED vpmulhw product is negative and vpackuswb's low-side
            // saturation zeroes it, exactly what the explicit max(0, ·) lands on — and
            // vpmulhw equals the unsigned mulhi on the remaining all-non-negative
            // operands (both factors < 2^15). Against the portable widening step this
            // drops four vpmaxsw, and narrows with two vpackuswb where that path paid
            // four vpmovwb plus two vinserti64x4 per 128 lanes. No cross-lane permute
            // follows the pack: the loader permutes every accumulator-side weight array
            // into the 512-bit packus order (network.c permute_packus_order), so the
            // pack's per-lane interleave IS the canonical output order.
            const __m512i ftmax = _mm512_set1_epi16(255);
            const __m512i sgnzero = _mm512_setzero_si512();
            const int16_t *in0 = comb_acc + base + j;
            const int16_t *in1 = comb_acc + base + j + half;
            // Unroll the two packs even though the 512-bit tiers build with
            // -fno-unroll-loops (build.sh). That flag is right for the big row and
            // search loops it was measured on and wrong for a two-trip loop whose body
            // is 13 instructions: rolled, clang re-derives both halves' displacements
            // per trip and emulates the trip count through a flag register, MEASURED at
            // ~10 scalar instructions per 64 lanes — more than the vector ops the
            // packus shape saves. Unrolled, every displacement is a literal.
    #pragma clang loop unroll(full)
            for (size_t k = 0; k < 2; k++) {
                const __m512i a0 = _mm512_loadu_si512((const void *) (in0 + 64 * k));
                const __m512i a1 = _mm512_loadu_si512((const void *) (in0 + 64 * k + 32));
                const __m512i b0 = _mm512_loadu_si512((const void *) (in1 + 64 * k));
                const __m512i b1 = _mm512_loadu_si512((const void *) (in1 + 64 * k + 32));
                const __m512i sum0a =
                  _mm512_slli_epi16(_mm512_max_epi16(_mm512_min_epi16(a0, ftmax), sgnzero), 7);
                const __m512i sum0b =
                  _mm512_slli_epi16(_mm512_max_epi16(_mm512_min_epi16(a1, ftmax), sgnzero), 7);
                const __m512i pa = _mm512_mulhi_epi16(sum0a, _mm512_min_epi16(b0, ftmax));
                const __m512i pb = _mm512_mulhi_epi16(sum0b, _mm512_min_epi16(b1, ftmax));
                const __m512i packed = _mm512_packus_epi16(pa, pb);
                // Store each pack where it lands — upstream's `*out++`, not a 128-byte
                // step temporary. The temporary is what the narrower tiers assemble
                // because their steps are unrolled and it costs nothing there; at 512
                // bits the build runs -fno-unroll-loops (build.sh), so the two halves
                // cannot live in registers across the k iterations and the temporary
                // becomes a real stack round trip: MEASURED at +1.8% whole-binary
                // instructions with it, against the portable step this replaces.
                _mm512_storeu_si512((void *) (output + offset + j + 64 * k), packed);
                // Harvest this half's non-zero-chunk bits while it is still a register —
                // upstream's NNZCursor::record2 shape, one step's mask stored at its own
                // offset. The u8 lanes are at most 126, so each dword group is a
                // non-negative i32 and greater-than-zero IS non-zero. AVX-512 compares
                // into a mask register, so this is one vpcmpd and one kmovw where the
                // narrower tiers pay a compare plus a movemask extract.
                const uint16_t mask16 = (uint16_t) _mm512_cmpgt_epi32_mask(packed, sgnzero);
                __builtin_memcpy(nnz_bytes + j / 32 + 2 * k, &mask16, sizeof mask16);
            }
#elif TRANSFORM_PACKUS_BITS == 256
            // Native avx2 product step, upstream's packus-clip shape (the
            // feature_transformer.h block comment): clamp the second operand from
            // above only — when it stays negative the SIGNED mulhi product is
            // negative and the saturating packus zeroes it, exactly the value the
            // explicit max(0, ·) path lands on — and mulhi_epi16 equals the
            // unsigned mulhi on the remaining all-non-negative operands (both
            // factors < 2^15). Four vpmaxsw per step drop out of the portable
            // lowering. No vpermq follows the pack: the loader permutes every
            // accumulator-side weight array into packus order (network.c
            // permute_packus_order), so the pack's in-lane interleave IS the
            // canonical output order — upstream's permute_weights shape.
            const __m256i ftmax = _mm256_set1_epi16(255);
            const __m256i sgnzero = _mm256_setzero_si256();
            const int16_t *in0 = comb_acc + base + j;
            const int16_t *in1 = comb_acc + base + j + half;
            TfU8 bytes;
            for (size_t k = 0; k < 2; k++) {
                const __m256i a0 = _mm256_loadu_si256((const __m256i *) (in0 + 32 * k));
                const __m256i a1 = _mm256_loadu_si256((const __m256i *) (in0 + 32 * k + 16));
                const __m256i b0 = _mm256_loadu_si256((const __m256i *) (in1 + 32 * k));
                const __m256i b1 = _mm256_loadu_si256((const __m256i *) (in1 + 32 * k + 16));
                const __m256i sum0a =
                  _mm256_slli_epi16(_mm256_max_epi16(_mm256_min_epi16(a0, ftmax), sgnzero), 7);
                const __m256i sum0b =
                  _mm256_slli_epi16(_mm256_max_epi16(_mm256_min_epi16(a1, ftmax), sgnzero), 7);
                const __m256i pa = _mm256_mulhi_epi16(sum0a, _mm256_min_epi16(b0, ftmax));
                const __m256i pb = _mm256_mulhi_epi16(sum0b, _mm256_min_epi16(b1, ftmax));
                const __m256i packed = _mm256_packus_epi16(pa, pb);
                __builtin_memcpy((unsigned char *) &bytes + 32 * k, &packed, sizeof packed);
                // Harvest the non-zero-chunk bits of this half while it is still a
                // register: the u8 lanes are at most 126, so each dword group is a
                // non-negative i32 and greater-than-zero IS non-zero. Store each
                // step's 8-bit vmovmskps result as its own byte — upstream's
                // NNZCursor::record2 shape. Combining the two masks into one u16
                // first hands clang a movemask-merge pattern it re-fuses into a
                // pack-and-shuffle chain (vpackssdw + vpacksswb + vpshufd +
                // vpmovmskb), two ops longer per step; the immediate byte store
                // keeps the two vpcmpgtd + vmovmskps pairs it stands for.
                // Consecutive steps cover disjoint byte spans and together the
                // whole bitset, so no zero-fill is needed, and little-endian byte
                // order makes byte (offset + j) / 32 + k exactly bits
                // [(offset + j) / 4, +16) of the u64 words.
                const uint32_t mask8 =
                  (uint32_t) _mm256_movemask_ps((__m256) _mm256_cmpgt_epi32(packed, sgnzero));
                nnz_bytes[j / 32 + k] = (uint8_t) mask8;
            }
#elif TRANSFORM_PACKUS_BITS == 128
            // Native sse41 product step, the same packus-clip shape as the avx2 branch
            // above: clamp only the FIRST operand from below — when the second stays
            // negative the SIGNED pmulhw product is negative and packuswb's low-side
            // saturation zeroes it, exactly the value the explicit max(0, ·) path lands
            // on — and pmulhw equals the unsigned mulhi on the remaining all-non-negative
            // operands (both factors < 2^15). Eight pmaxsw per step drop out of the
            // portable lowering, which clamps both operands. Unlike avx2, no weight
            // permute is needed: at 128 bits packuswb concatenates its operands in
            // order, so the packed bytes are already the canonical output order.
            const __m128i ftmax = _mm_set1_epi16(255);
            const __m128i sgnzero = _mm_setzero_si128();
            const int16_t *in0 = comb_acc + base + j;
            const int16_t *in1 = comb_acc + base + j + half;
            TfU8 bytes;
            for (size_t k = 0; k < 4; k++) {
                const __m128i a0 = _mm_loadu_si128((const __m128i *) (in0 + 16 * k));
                const __m128i a1 = _mm_loadu_si128((const __m128i *) (in0 + 16 * k + 8));
                const __m128i b0 = _mm_loadu_si128((const __m128i *) (in1 + 16 * k));
                const __m128i b1 = _mm_loadu_si128((const __m128i *) (in1 + 16 * k + 8));
                const __m128i sum0a =
                  _mm_slli_epi16(_mm_max_epi16(_mm_min_epi16(a0, ftmax), sgnzero), 7);
                const __m128i sum0b =
                  _mm_slli_epi16(_mm_max_epi16(_mm_min_epi16(a1, ftmax), sgnzero), 7);
                const __m128i pa = _mm_mulhi_epi16(sum0a, _mm_min_epi16(b0, ftmax));
                const __m128i pb = _mm_mulhi_epi16(sum0b, _mm_min_epi16(b1, ftmax));
                const __m128i packed = _mm_packus_epi16(pa, pb);
                __builtin_memcpy((unsigned char *) &bytes + 16 * k, &packed, sizeof packed);
            }
#else
            const TfI16 s0 = tf_i16_load(comb_acc + base + j);
            const TfI16 s1 = tf_i16_load(comb_acc + base + j + half);
            const TfU16 c0 = tf_i16_to_u16(tf_i16_max(zero, tf_i16_min(c255, s0)));
            const TfU16 c1 = tf_i16_to_u16(tf_i16_max(zero, tf_i16_min(c255, s1)));
            const TfU32 lhs = tf_u16_to_u32(tf_u16_shl(c0, 7));
            const TfU32 rhs = tf_u16_to_u32(c1);
            const TfU16 q = tf_u32_to_u16(tf_u32_shr(tf_u32_mul(lhs, rhs), 16));
            const TfU8 bytes = tf_u16_to_u8(q);
#endif
#if TRANSFORM_PACKUS_BITS != 512
            // The 512-bit step above already stored each pack at its own offset.
            tf_u8_store(output + offset + j, bytes);
#endif

#if TRANSFORM_PACKUS_BITS < 256
            // Record which 4-byte chunks are non-zero while they are still in a register:
            // no reload of what was just stored. (The 512- and 256-bit branches above
            // already stored their per-pack mask bytes.)
            const TfGroups groups = tf_u8_as_groups(bytes);

            // Build the non-zero-chunk mask with one horizontal movemask (compare-to-zero
            // + reduce-OR) rather than a per-lane extract+compare+shift+OR each group.
            const uint64_t mask = tf_movemask(groups);
            const size_t bit = (offset + j) / 4;
    #if defined(__AVX512F__) || (MCFISH_SIMD_VECTOR && defined(__SSE2__))
                // Store the step's mask directly at its own byte offset — upstream's
                // `*out++` NNZCursor shape. Consecutive steps cover disjoint byte spans
                // and together the whole bitset, so no zero-fill precedes and no other
                // step's word is read back: the little-endian byte store IS the
                // `word |= mask << (bit % 64)` the scalar fallback keeps, which is why
                // this stays behind the x86 guards.
        #if defined(__AVX512F__)
            const uint32_t mask_bits = (uint32_t) mask;
        #else
            const uint16_t mask_bits = (uint16_t) mask;
        #endif
            // Select the store's width by the SAME macro that selects the tile width
            // above (the width enum is invisible to the preprocessor), and pin the
            // pairing: a store narrower than the step's mask silently drops bits.
            static_assert(TRANSFORM_VEC_WIDTH / 4 == 8 * sizeof mask_bits,
                          "each step's mask must fill its stored span exactly");
            __builtin_memcpy((unsigned char *) *nnz + bit / 8, &mask_bits, sizeof mask_bits);
    #else
            (*nnz)[bit / 64] |= mask << (bit % 64);
    #endif
#endif
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

    // The threat slot holds only its diff -- there is no threat computed flag to clear
    // (find_last_usable consults only the psq slot, so a threat computed byte was always a
    // dead write and no longer fits the tightened stride).
    zero_diff(bytes, THREAT_FEATURE, 0, sizeof(NnueDirtyThreats));

    set_stack_size(bytes, 1);
}

NnueStackPushOutput nnue_acc_stack_push(NnueAccumulatorStack *stack) {
    unsigned char *bytes = stack_bytes_mut(stack);
    const size_t index = stack_size(stack);
    assert(index < NNUE_MAX_STACK_SIZE);

    clear_computed(bytes, PSQ_FEATURE, index);

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
