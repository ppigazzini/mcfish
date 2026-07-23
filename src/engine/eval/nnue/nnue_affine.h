// Own the NNUE affine (fully-connected) layer kernel and the two activations that follow
// it: ClippedReLU and SqrClippedReLU.
//
// The affine is a pure integer dot product of uint8 inputs against int8 weights, in the
// SSSE3-SCRAMBLED weight layout: the weight of output j, input group g, sublane m lives
// at `g * OUT * 4 + j * 4 + m`. Groups are four consecutive inputs. That layout is the
// net file's, not a choice made here.
//
// The INVARIANT is that the accumulation is exact int32 with no rounding and no overflow
// — inputs are bounded by 127 and weights by 128, so a layer's largest partial sum is far
// inside int32. Integer addition therefore commutes, and the kernel is free to accumulate
// in the interleaved (OUT*4) domain and fold each output's four sublanes together once at
// the end. That reassociation is what lets the whole kernel be element-wise, so the
// vector and scalar paths of simd.h produce identical results.
//
// Sparse layers walk the transform's NNZ bitset instead of every group. Indices ascend
// either way, so skipping an all-zero group removes only terms that are zero.
//
// Upstream reaches the same dot through arch-tiered instruction selections (vpdpbusd,
// pmaddubsw+pmaddwd, and a portable vpmaddwd deinterleave); all of them compute this same
// sum, so the tier is a performance choice and is not reproduced here.
//
// Golden: the upstream `nnue/layers/affine_transform.h`,
// `nnue/layers/affine_transform_sparse_input.h` and `nnue/layers/clipped_relu.h`.

#ifndef MCFISH_NNUE_AFFINE_H
#define MCFISH_NNUE_AFFINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nnue_ft.h"

enum {
    // Match the transform's bitset width: one bit per 4-byte output chunk.
    NNUE_AFFINE_NNZ_WORD_COUNT = NNUE_HALF_DIMENSIONS / 4 / 64,
};

// Compute the 32-output affine layer. INPUT_LEN must be a multiple of 4. WEIGHTS holds
// INPUT_LEN * 32 int8 entries in the scrambled layout and must be 16-BYTE ALIGNED: the
// SSSE3 tier of nnue_dot_step loads each 16-byte row chunk with an aligned load so it
// folds into pmaddubsw's memory operand, mirroring upstream's alignas(CacheLineSize)
// weights (affine_transform.h). Both callers pass nnue_layer_storage blocks, which are
// at least 64-byte aligned. NNZ is read only when SPARSE.
void nnue_affine_32(bool sparse,
                    int32_t out[32],
                    const int32_t *biases,
                    const int8_t *weights,
                    const uint8_t *input,
                    size_t input_len,
                    const uint64_t *nnz);

// Compute the single-output affine layer, where the scramble is the identity. The dense
// path requires INPUT and WEIGHTS 16-byte aligned (see nnue_affine1_dot in simd.h).
void nnue_affine_1(bool sparse,
                   int32_t out[1],
                   const int32_t *biases,
                   const int8_t *weights,
                   const uint8_t *input,
                   size_t input_len,
                   const uint64_t *nnz);

// Compute upstream's ClippedReLU over 32 outputs: clamp(x >> SHIFT, 0, 127).
//
// `>>` on a negative value is an arithmetic shift, which C23 requires and which both
// compilers' vector `>>` already do.
void nnue_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]);

// Compute upstream's SqrClippedReLU over 32 outputs: min(127, (x*x) >> SHIFT).
//
// Clamping x into int16 range first is what keeps the square inside int32, and it is
// exact rather than an approximation: any x outside int16 squares past the 127 cap
// regardless. That is the property upstream's saturating `packs_epi32` relies on.
void nnue_sqr_clipped_relu_32(int shift, const int32_t in[32], uint8_t out[32]);

#endif  // MCFISH_NNUE_AFFINE_H
