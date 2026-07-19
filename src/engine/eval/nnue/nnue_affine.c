// Implement the NNUE affine layer and its two activations. See nnue_affine.h for the
// scrambled weight layout and the exactness invariant the reassociation rests on.
//
// Ported from zfish `engine/eval/nnue_affine.zig` and `nnue_inference.zig`.

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

// Accumulate the 32 outputs as EIGHT vectors of four int32 rather than one vector of
// 128. Both hold the same 32 sums; the difference is that this shape folds each
// output's four sublanes inside the multiply (nnue_dot4_i32) instead of widening to
// int32 first and folding at the end. 128 int32 lanes is 512 bytes against a 256-byte
// SSE register file, so the wide accumulator spilled and reloaded on every group --
// which was the whole nps deficit against zfish, not a constant factor on it.
//
// Chunk c covers weight bytes [c*16, c*16+16) of the group. The scrambled layout
// (g*OUT*4 + j*4 + m) makes that exactly outputs 4c..4c+3 across all four sublanes, so
// chunk c's four result lanes ARE outputs 4c..4c+3 and no shuffle is needed anywhere.
enum { AFFINE_CHUNKS_32 = 8 };

static inline void
group_accumulate_32(NnueV4i32 acc[AFFINE_CHUNKS_32], uint32_t packed, const int8_t *weights) {
    // Broadcast the group's four bytes across sixteen, so byte 4q+s is sublane s for
    // every q. The uint32 round trip keeps this endianness-neutral: the same four bytes
    // land in the same four positions on either byte order.
    const NnueV16u8 in = nnue_v16_u32x4_as_u8(nnue_v4u32_splat(packed));
    for (size_t c = 0; c < AFFINE_CHUNKS_32; c++) {
        acc[c] = nnue_v4i32_add(acc[c], nnue_dot4_i32(in, nnue_v16i8_load(weights + c * 16)));
    }
}

void nnue_affine_32(bool sparse,
                    int32_t out[32],
                    const int32_t *biases,
                    const int8_t *weights,
                    const uint8_t *input,
                    size_t input_len,
                    const uint64_t *nnz) {
    enum { OUT = 32, N = OUT * 4 };
    const size_t groups = input_len / 4;
    NnueV4i32 acc[AFFINE_CHUNKS_32];
    for (size_t c = 0; c < AFFINE_CHUNKS_32; c++)
        acc[c] = nnue_v4i32_splat(0);

    if (sparse) {
        // Walk the bitset in upstream's shape (affine_transform_sparse_input.h): load a
        // whole 64-group word, hoist the input and weight bases ONCE per word, then pop
        // set bits with a LOCAL index rather than re-scaling an absolute group index.
        for (size_t k = 0; k * 64 < groups; k++) {
            uint64_t bits = nnz[k];
            const uint8_t *in_base = input + k * 64 * 4;
            const int8_t *w_base = weights + k * 64 * N;
            while (bits != 0) {
                const size_t i = (size_t) __builtin_ctzll(bits);
                bits &= bits - 1;
                group_accumulate_32(acc, load_group(in_base + i * 4), w_base + i * N);
            }
        }
    } else {
        for (size_t g = 0; g < groups; g++) {
            group_accumulate_32(acc, load_group(input + g * 4), weights + g * N);
        }
    }

    // The sublane fold already happened inside nnue_dot4_i32, so each lane is a finished
    // dot product and only the bias remains.
    for (size_t j = 0; j < OUT; j++) {
        out[j] = biases[j] + nnue_v4i32_lane(acc[j / 4], j % 4);
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
    NnueV4i32 acc = nnue_v4i32_splat(0);

    if (sparse) {
        for (size_t k = 0; k * 64 < groups; k++) {
            uint64_t bits = nnz[k];
            const uint8_t *in_base = input + k * 64 * 4;
            const int8_t *w_base = weights + k * 64 * N;
            while (bits != 0) {
                const size_t i = (size_t) __builtin_ctzll(bits);
                bits &= bits - 1;
                acc = nnue_v4i32_add(acc,
                                     group_products_1(load_group(in_base + i * 4), w_base + i * N));
            }
        }
    } else {
        for (size_t g = 0; g < groups; g++) {
            acc = nnue_v4i32_add(acc, group_products_1(load_group(input + g * 4), weights + g * N));
        }
    }

    out[0] = biases[0] + nnue_v4i32_lane(acc, 0) + nnue_v4i32_lane(acc, 1) + nnue_v4i32_lane(acc, 2)
           + nnue_v4i32_lane(acc, 3);
}

// --- activations -------------------------------------------------------------------

enum { RELU_VEC_WIDTH = 8 };

void nnue_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]) {
    const NnueV8i32 zero = nnue_v8i32_splat(0);
    const NnueV8i32 cap = nnue_v8i32_splat(127);
    for (size_t i = 0; i < 32; i += RELU_VEC_WIDTH) {
        const NnueV8i32 x = nnue_v8i32_load(in + i);
        const NnueV8i32 q = nnue_v8i32_max(zero, nnue_v8i32_min(cap, nnue_v8i32_shr(x, shift)));
        for (size_t k = 0; k < RELU_VEC_WIDTH; k++) {
            out[i + k] = (uint8_t) nnue_v8i32_lane(q, k);
        }
    }
}

void nnue_sqr_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]) {
    const NnueV8i32 lo = nnue_v8i32_splat(-32768);
    const NnueV8i32 hi = nnue_v8i32_splat(32767);
    const NnueV8i32 cap = nnue_v8i32_splat(127);
    for (size_t i = 0; i < 32; i += RELU_VEC_WIDTH) {
        const NnueV8i32 x = nnue_v8i32_load(in + i);
        const NnueV8i32 clamped = nnue_v8i32_max(lo, nnue_v8i32_min(hi, x));
        const NnueV8i32 q =
          nnue_v8i32_min(cap, nnue_v8i32_shr(nnue_v8i32_mul(clamped, clamped), shift));
        for (size_t k = 0; k < RELU_VEC_WIDTH; k++) {
            out[i + k] = (uint8_t) nnue_v8i32_lane(q, k);
        }
    }
}
