// Own the NNUE forward pass: the accumulator transform followed by the per-bucket affine
// stack, for SFNNv15.
//
// The INVARIANT is that this module is pure compute. It reads the loaded weights and the
// position, writes nothing but the caller's accumulator stack and refresh cache, and has
// no file I/O — so the net loader and the evaluation can be ported and gated separately.
//
// The layer stack, per bucket: fc_0 (affine 1024 -> 32, sparse) -> {ac_sqr_0, ac_0} ->
// fc_1 (affine 64 -> 32) -> {ac_sqr_1, ac_1} -> fc_2 (affine 128 -> 1), plus the skip term
// fc_0_out[30] - fc_0_out[31]. Weights are int8 in the scrambled layout, biases int32
// linear, WeightScaleBits = 6.
//
// Ported from zfish `engine/eval/nnue_inference.zig` against the upstream golden
// `nnue/network.cpp`.

#ifndef MCFISH_NNUE_INFERENCE_H
#define MCFISH_NNUE_INFERENCE_H

#include <stddef.h>
#include <stdint.h>

#include "../../board/position.h"
#include "nnue_accumulator.h"
#include "nnue_ft.h"

// NNUE_LAYER_STACKS / NNUE_LAYERS_PER_STACK come from nnue_architecture.h,
// the single authority for the network dimensions.

typedef struct NnueEvalOutput {
    int32_t psqt;
    int32_t positional;
} NnueEvalOutput;

typedef struct NnueTraceOutput {
    int32_t psqt[NNUE_LAYER_STACKS];
    int32_t positional[NNUE_LAYER_STACKS];
    size_t correct_bucket;
} NnueTraceOutput;

// --- the weight-storage seam ------------------------------------------------------
//
// `nnue_weight_storage` hands its arenas out untyped; these two put the layer arrays back
// into the types the forward pass reads them as. Both return nullptr until a net is
// resident, and the forward pass does NOT check — a caller must have loaded a net before
// evaluating.

// Return the int32 bias array of layer IDX in BUCKET's stack.
const int32_t *nnue_layer_biases(size_t bucket, size_t idx);

// Return the int8 weight array of layer IDX in BUCKET's stack, scrambled layout.
const int8_t *nnue_layer_weights(size_t bucket, size_t idx);

// ----------------------------------------------------------------------------------

// Evaluate POS through the bucket its piece count selects.
NnueEvalOutput
nnue_inference_evaluate(const Position *pos, NnueAccumulatorStack *stack, NnueRefreshCache *cache);

// Evaluate POS through EVERY bucket, for the `eval` trace, and name the one the piece
// count would have selected.
NnueTraceOutput nnue_inference_trace_evaluate(const Position *pos,
                                              NnueAccumulatorStack *stack,
                                              NnueRefreshCache *cache);

#endif  // MCFISH_NNUE_INFERENCE_H
