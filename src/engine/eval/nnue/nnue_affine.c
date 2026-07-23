// Implement the NNUE affine layer and its two activations. See nnue_affine.h for the
// scrambled weight layout and the exactness invariant the reassociation rests on.

#include "nnue_affine.h"

#include <string.h>

#include "simd.h"

// Broadcast one input group's four bytes across an OUT*4-lane vector, so lane k carries
// input sublane k % 4 — which is exactly what the scrambled weight layout wants. Built by
// a uint32 round trip so it is endianness-neutral: the same four bytes come back in the
// same four lanes on either byte order.
static inline uint32_t load_group(const uint8_t *input) {
    uint32_t packed;
    memcpy(&packed, input, sizeof packed);
    return packed;
}

// --- OUT = 32 (N = 128 interleaved lanes) -----------------------------------------

// Accumulate the 32 outputs in NNUE_DOT_LANES-wide vectors, folding each output's
// four sublanes inside the multiply rather than widening to int32 first. Holding the
// interleaved OUT*4 domain instead needs 128 int32 lanes -- 512 bytes against a
// 256-byte SSE register file -- so that accumulator spilled and reloaded on every
// group, which was the whole nps deficit rather than a constant factor on it.
//
// Chunk c covers weight bytes [c*LANES*4, +LANES*4) of the group. The scrambled
// layout (g*OUT*4 + j*4 + m) makes that exactly outputs c*LANES..c*LANES+LANES-1
// across all four sublanes, so a chunk's result lanes ARE those outputs and no
// shuffle is needed at any width.
//
// CHAINS breaks the loop-carried dependency. vpdpbusd has several cycles of latency
// and a single accumulator would serialise the entire group walk on it; rotating
// over independent accumulators lets the out-of-order engine overlap them, and they
// are summed once at the end. Every group lands in exactly one chain and integer
// addition commutes, so the split cannot change the result. The chain index is a
// literal at each call site -- a runtime counter would index the array dynamically
// and spill it to memory, which is the thing this shape exists to avoid.
enum {
    AFFINE_CHUNKS_32 = 32 / NNUE_DOT_LANES,

// Chains cost registers: the accumulator is CHUNKS * CHAINS vectors, and it must
// stay resident or the spilling this whole shape exists to avoid comes back.
// Gate the chain count on the register file the tier leaves free, in the spirit
// of upstream's per-ISA NumRegs (affine_transform_sparse_input.h). Under
// AVX512-VNNI 3 chains is 6 of 32 zmm — upstream's exact choice. At plain AVX2
// 3 chains is 12 of 16 ymm held live across the sparse walk, which spills
// (callgrind attributes the reload traffic to no source line); 2 chains keeps
// the block resident and still overlaps the maddubs->madd->add dependency.
// A 4-lane tier already needs 8 chunks and takes one chain for the same reason.
#if defined(__AVX512VNNI__)
    AFFINE_CHAINS = 3,
#elif defined(__AVXVNNI__) || defined(__AVX2__)
    AFFINE_CHAINS = 2,
#else
    AFFINE_CHAINS = 1,
#endif
};

typedef struct {
    NnueDotAcc c[AFFINE_CHUNKS_32][AFFINE_CHAINS];
} AffineAcc32;

// Seed chain 0 of each chunk with the layer bias vector and zero the rest, so the
// bias enters the int32 accumulator up front instead of a per-lane scalar add at the
// tail (upstream seeds acc[k] from biasvec, affine_transform.h:277-282). The bias
// never touches the pmaddubsw int16 intermediate, and integer add commutes, so this
// is bit-identical to zero-then-add-bias and scalar==vector holds.
static inline void affine_acc32_seed_bias(AffineAcc32 *a, const int32_t *biases) {
    for (size_t i = 0; i < AFFINE_CHUNKS_32; i++) {
        a->c[i][0] = nnue_dot_load(biases + i * NNUE_DOT_LANES);
        for (size_t k = 1; k < AFFINE_CHAINS; k++)
            a->c[i][k] = nnue_dot_zero();
    }
}

// Fold one input group into chain K, which is a literal at every call site.
#define AFFINE_GROUP_INTO(acc, K, packed, weights) \
    do { \
        for (size_t c_ = 0; c_ < AFFINE_CHUNKS_32; c_++) \
            (acc)->c[c_][(K)] = \
              nnue_dot_step((acc)->c[c_][(K)], (packed), (weights) + c_ * NNUE_DOT_LANES * 4); \
    } while (0)

void nnue_affine_32(bool sparse,
                    int32_t out[32],
                    const int32_t *biases,
                    const int8_t *weights,
                    const uint8_t *input,
                    size_t input_len,
                    const uint64_t *nnz) {
    enum { OUT = 32, N = OUT * 4 };
    const size_t groups = input_len / 4;
    AffineAcc32 acc;
    affine_acc32_seed_bias(&acc, biases);

    if (sparse) {
        // Walk the bitset in upstream's shape (affine_transform_sparse_input.h): load a
        // whole 64-group word, hoist the input and weight bases ONCE per word, then pop
        // set bits with a LOCAL index rather than re-scaling an absolute group index.
        //
        // On the avx2 tier only, unroll the word walk fully where the trip count folds
        // to a constant: the one sparse caller (fc_0) passes a literal input_len, so
        // after LTO inlining this is four copies whose in_base/w_base are fixed
        // displacements — upstream's shape, whose four word loops carry no per-word
        // base recompute. In the standalone (runtime-count) compile of this function
        // the pragma is ignored. Measured (perf_counters, bench 16 1 13): the
        // recompute skeleton was 56 of fc_0's ~1812 instructions per evaluation and
        // the unroll removes most of it at avx2, but the SAME unroll REGRESSES the
        // 16-lane native tier, where the wider per-word state spills — so the pragma
        // is tier-guarded, not unconditional.
#if MCFISH_SIMD_VECTOR && defined(__AVX2__) && !defined(__AVX512F__)
    #pragma clang loop unroll(full)
#endif
        for (size_t k = 0; k * 64 < groups; k++) {
            uint64_t bits = nnz[k];
            const uint8_t *in_base = input + k * 64 * 4;
            const int8_t *w_base = weights + k * 64 * N;

            while (bits != 0) {
                const size_t i0 = (size_t) __builtin_ctzll(bits);
                bits &= bits - 1;
                AFFINE_GROUP_INTO(&acc, 0, load_group(in_base + i0 * 4), w_base + i0 * N);
                if (bits == 0)
                    break;
                const size_t i1 = (size_t) __builtin_ctzll(bits);
                bits &= bits - 1;
                AFFINE_GROUP_INTO(&acc, 1 % AFFINE_CHAINS, load_group(in_base + i1 * 4),
                                  w_base + i1 * N);
                if (bits == 0)
                    break;
                const size_t i2 = (size_t) __builtin_ctzll(bits);
                bits &= bits - 1;
                AFFINE_GROUP_INTO(&acc, 2 % AFFINE_CHAINS, load_group(in_base + i2 * 4),
                                  w_base + i2 * N);
            }
        }
    } else if (AFFINE_CHAINS > 1) {
        size_t g = 0;
        for (; g + 3 <= groups; g += 3) {
            AFFINE_GROUP_INTO(&acc, 0, load_group(input + g * 4), weights + g * N);
            AFFINE_GROUP_INTO(&acc, 1 % AFFINE_CHAINS, load_group(input + (g + 1) * 4),
                              weights + (g + 1) * N);
            AFFINE_GROUP_INTO(&acc, 2 % AFFINE_CHAINS, load_group(input + (g + 2) * 4),
                              weights + (g + 2) * N);
        }
        for (; g < groups; g++)
            AFFINE_GROUP_INTO(&acc, 0, load_group(input + g * 4), weights + g * N);
    } else {
        // One chain: the 3-group unroll above re-feeds the SAME accumulator, so it buys
        // no latency overlap and only widens the live range past the 16-register SSE
        // file -- clang spills accumulator chunks mid-loop for it. Upstream's non-VNNI
        // dense walk is one group per iteration (affine_transform.h propagate); match it.
        for (size_t g = 0; g < groups; g++)
            AFFINE_GROUP_INTO(&acc, 0, load_group(input + g * 4), weights + g * N);
    }

    // Chain 0 was bias-seeded, so each lane already holds bias + Σ dot products -- the
    // finished output. Merge the chains and store the whole register, rather than
    // extracting and scalar-storing 32 lanes with a per-lane bias add.
    for (size_t c = 0; c < AFFINE_CHUNKS_32; c++) {
        NnueDotAcc merged = acc.c[c][0];
        for (size_t k = 1; k < AFFINE_CHAINS; k++)
            merged = nnue_dot_add(merged, acc.c[c][k]);
        nnue_dot_store(out + c * NNUE_DOT_LANES, merged);
    }
}

// --- OUT = 1 (N = 4) ---------------------------------------------------------------

static inline NnueV4i32 group_products_1(uint32_t packed, const int8_t *weights) {
    const NnueV4i32 in = nnue_v4_u8_to_i32(nnue_v4_u32x1_as_u8(nnue_v1u32_splat(packed)));
    const NnueV4i32 w = nnue_v4_i8_to_i32(nnue_v4i8_load(weights));
    return nnue_v4i32_mul(in, w);
}

void nnue_affine_1(bool sparse,
                   int32_t out[1],
                   const int32_t *biases,
                   const int8_t *weights,
                   const uint8_t *input,
                   size_t input_len,
                   const uint64_t *nnz) {
    enum { N = 4 };
    const size_t groups = input_len / 4;

    // fc_2 is the sole OUT==1 layer and always runs dense. Its N=4 weight layout is
    // the identity permutation, so the whole layer is a contiguous 128-wide u8*i8 dot
    // (nnue_affine1_dot); the interleaved-group form below is bit-identical to it but
    // only survives for the sparse contract other OUT==1 callers could carry.
    if (!sparse) {
        out[0] = biases[0] + nnue_affine1_dot(input, weights, input_len);
        return;
    }

    NnueV4i32 acc = nnue_v4i32_splat(0);
    for (size_t k = 0; k * 64 < groups; k++) {
        uint64_t bits = nnz[k];
        const uint8_t *in_base = input + k * 64 * 4;
        const int8_t *w_base = weights + k * 64 * N;
        while (bits != 0) {
            const size_t i = (size_t) __builtin_ctzll(bits);
            bits &= bits - 1;
            acc =
              nnue_v4i32_add(acc, group_products_1(load_group(in_base + i * 4), w_base + i * N));
        }
    }

    out[0] = biases[0] + nnue_v4i32_lane(acc, 0) + nnue_v4i32_lane(acc, 1) + nnue_v4i32_lane(acc, 2)
           + nnue_v4i32_lane(acc, 3);
}

// --- activations -------------------------------------------------------------------

#if MCFISH_SIMD_VECTOR && defined(__SSE4_1__) && !defined(__AVX512F__)
    #include <smmintrin.h>

// Native SSE bodies for the two activations, upstream's shape (clipped_relu.h,
// sqr_clipped_relu.h): run the clamp-shift-narrow in the 16-bit pack domain, where the
// saturating packs ARE the clamps, instead of the portable 32-bit min/max/pmulld chain
// the vector extensions lower to. Upstream has NO wider x86 branch here — its avx2
// build runs exactly these 128-bit instructions — and the avx2 profile agrees: the
// portable body costs ~100 instructions per evaluation (vpminsd/vpmaxsd/2-uop vpmulld
// plus a pack-and-permute narrow) against ~30 for these. Only the avx512 tier keeps
// the portable body, where vpmovdb narrows a whole zmm in one op and inference already
// measures ahead of the oracle. Bit-identity per body below; simd-scalar and the
// oracle's own SSE build (which runs exactly these instructions) both pin it.

// min(127, (clamp(x, -32768, 32767)^2) >> SHIFT), SHIFT > 16, over 32 lanes.
//   - packs_epi32 saturates int32 -> int16: exactly the explicit [-32768, 32767] clamp.
//   - pmulhi(w, w) = (w*w) >> 16, exact: w*w <= 2^30 < 2^31 never overflows int32.
//   - srli by SHIFT-16 on a value <= 2^14: (w*w >> 16) >> (SHIFT-16) == w*w >> SHIFT,
//     floor-of-floor composes.
//   - packs_epi16 saturates to [-128, 127]: every lane is in [0, 2^30 >> SHIFT], so
//     this is exactly min(127, .).
void nnue_sqr_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]) {
    for (size_t i = 0; i < 32; i += 16) {
        const __m128i a = _mm_loadu_si128((const __m128i *) (const void *) (in + i));
        const __m128i b = _mm_loadu_si128((const __m128i *) (const void *) (in + i + 4));
        const __m128i c = _mm_loadu_si128((const __m128i *) (const void *) (in + i + 8));
        const __m128i d = _mm_loadu_si128((const __m128i *) (const void *) (in + i + 12));
        const __m128i w0 = _mm_packs_epi32(a, b);
        const __m128i w1 = _mm_packs_epi32(c, d);
        const __m128i h0 = _mm_srli_epi16(_mm_mulhi_epi16(w0, w0), shift - 16);
        const __m128i h1 = _mm_srli_epi16(_mm_mulhi_epi16(w1, w1), shift - 16);
        _mm_storeu_si128((__m128i *) (void *) (out + i), _mm_packs_epi16(h0, h1));
    }
}

// clamp(x >> SHIFT, 0, 127) with an arithmetic shift, over 32 lanes.
//   - packus_epi32 saturates int32 -> uint16 = clamp(x, 0, 65535): for x < 0 both give
//     0 (arithmetic shift keeps the sign, then the max-with-0 floors it); for
//     x > 65535 both saturate past the 127 cap regardless of SHIFT <= 9.
//   - srli by SHIFT on the unsigned clamp equals the arithmetic shift on [0, 65535].
//   - packs_epi16 saturates to 127: lanes are in [0, 65535 >> SHIFT].
void nnue_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]) {
    for (size_t i = 0; i < 32; i += 16) {
        const __m128i a = _mm_loadu_si128((const __m128i *) (const void *) (in + i));
        const __m128i b = _mm_loadu_si128((const __m128i *) (const void *) (in + i + 4));
        const __m128i c = _mm_loadu_si128((const __m128i *) (const void *) (in + i + 8));
        const __m128i d = _mm_loadu_si128((const __m128i *) (const void *) (in + i + 12));
        const __m128i h0 = _mm_srli_epi16(_mm_packus_epi32(a, b), shift);
        const __m128i h1 = _mm_srli_epi16(_mm_packus_epi32(c, d), shift);
        _mm_storeu_si128((__m128i *) (void *) (out + i), _mm_packs_epi16(h0, h1));
    }
}

#else  // portable activation bodies

enum { RELU_VEC_WIDTH = 16 };

// Narrow sixteen int32 lanes to bytes and store them as a unit.
//
// Extracting lane by lane emits sixteen scalar byte stores per call, and the two
// activations run four times per evaluation. Every value is already clamped into
// [0, 127] by the caller, so the narrowing is exact and a shuffle-based pack is
// value-identical to reading the lanes out one at a time -- which is what lets the
// vector and scalar bodies stay equal.
static inline void relu_store16(uint8_t *out, NnueV16i32 q) {
    #if MCFISH_SIMD_VECTOR
    // __builtin_convertvector narrows all sixteen lanes in one expression; the
    // compiler picks the pack sequence for whatever ISA is enabled.
    typedef uint8_t NnueV16u8Store __attribute__((vector_size(16)));
    const NnueV16u8Store packed = __builtin_convertvector(q, NnueV16u8Store);
    __builtin_memcpy(out, &packed, sizeof packed);
    #else
    for (size_t k = 0; k < RELU_VEC_WIDTH; k++)
        out[k] = (uint8_t) nnue_v16i32_lane(q, k);
    #endif
}

void nnue_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]) {
    const NnueV16i32 zero = nnue_v16i32_splat(0);
    const NnueV16i32 cap = nnue_v16i32_splat(127);
    for (size_t i = 0; i < 32; i += RELU_VEC_WIDTH) {
        const NnueV16i32 x = nnue_v16i32_load(in + i);
        const NnueV16i32 q = nnue_v16i32_max(zero, nnue_v16i32_min(cap, nnue_v16i32_shr(x, shift)));
        relu_store16(out + i, q);
    }
}

void nnue_sqr_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]) {
    const NnueV16i32 lo = nnue_v16i32_splat(-32768);
    const NnueV16i32 hi = nnue_v16i32_splat(32767);
    const NnueV16i32 cap = nnue_v16i32_splat(127);
    for (size_t i = 0; i < 32; i += RELU_VEC_WIDTH) {
        const NnueV16i32 x = nnue_v16i32_load(in + i);
        const NnueV16i32 clamped = nnue_v16i32_max(lo, nnue_v16i32_min(hi, x));
        const NnueV16i32 q =
          nnue_v16i32_min(cap, nnue_v16i32_shr(nnue_v16i32_mul(clamped, clamped), shift));
        relu_store16(out + i, q);
    }
}

#endif  // activation bodies
