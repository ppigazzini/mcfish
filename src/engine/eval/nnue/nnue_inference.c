// Run the NNUE forward pass. See nnue_inference.h for the layer stack and the seam this
// module reads its weights through.

#include "nnue_inference.h"

#include "nnue_weight_storage.h"

#include <string.h>

#include "nnue_affine.h"

static_assert((int) NNUE_AFFINE_NNZ_WORD_COUNT == (int) NNUE_NNZ_WORD_COUNT,
              "the affine and the transform must agree on the NNZ bitset width");

enum { OUTPUT_SCALE = 16, CACHE_LINE_SIZE = 64 };

// Bridge the inference layer to weight storage. nnue_weight_storage.c owns the bytes and
// hands them out untyped (nnue_layer_ptr); inference wants them typed. The cast is sound
// because the storage is allocated 64-byte aligned and the parser writes biases as native
// int32_t and weights as int8_t in scrambled order — see nnue_parse.c.
const int32_t *nnue_layer_biases(size_t bucket, size_t idx) {
    const uint8_t *p = nnue_layer_ptr(bucket, idx, NNUE_LAYER_BIASES);
    return (const int32_t *) (const void *) p;
}

const int8_t *nnue_layer_weights(size_t bucket, size_t idx) {
    const uint8_t *p = nnue_layer_ptr(bucket, idx, NNUE_LAYER_WEIGHTS);
    return (const int8_t *) (const void *) p;
}

// Run one bucket's affine stack over the transformed features.
//
// The activation shifts are upstream's, per layer: fc_0 feeds
// WeightScaleBitsLocal = WeightScaleBits + 1 = 7, so SqrClippedReLU shifts by 2*7+7 = 21
// and ClippedReLU by 7; fc_1 feeds WeightScaleBitsLocal = 6, so 2*6+7 = 19 and 6.
static int32_t
propagate_bucket(size_t bucket, const uint8_t *transformed, const NnueNnzBitset *nnz) {
    const int32_t *fc0_b = nnue_layer_biases(bucket, 0);
    const int8_t *fc0_w = nnue_layer_weights(bucket, 0);
    const int32_t *fc1_b = nnue_layer_biases(bucket, 1);
    const int8_t *fc1_w = nnue_layer_weights(bucket, 1);
    const int32_t *fc2_b = nnue_layer_biases(bucket, 2);
    const int8_t *fc2_w = nnue_layer_weights(bucket, 2);

    // fc_0: affine 1024 -> 32, sparse over the transform's NNZ bitset.
    int32_t fc0_out[32];
    nnue_affine_32(true, fc0_out, fc0_b, fc0_w, transformed, NNUE_TRANSFORMED_BYTES, *nnz);

    // Build SFNNv15 concat[128] = [ac_sqr_0(32) | ac_0(32) | ac_sqr_1(32) | ac_1(32)].
    uint8_t concat[128] = { 0 };
    nnue_sqr_clipped_relu_32(21, fc0_out, concat + 0);
    nnue_clipped_relu_32(7, fc0_out, concat + 32);

    // fc_1: affine 64 -> 32 over [ac_sqr_0 | ac_0].
    int32_t fc1_out[32];
    nnue_affine_32(false, fc1_out, fc1_b, fc1_w, concat, 64, *nnz);

    nnue_sqr_clipped_relu_32(19, fc1_out, concat + 64);
    nnue_clipped_relu_32(6, fc1_out, concat + 96);

    // fc_2: affine 128 -> 1 over the full concat.
    int32_t fc2_out[1];
    nnue_affine_1(false, fc2_out, fc2_b, fc2_w, concat, 128, *nnz);

    // fwdOut = fc_2_out[0] + (fc_0_out[FC_0_OUTPUTS-2] - fc_0_out[FC_0_OUTPUTS-1]), scaled
    // by 600 * OutputScale / (HiddenOneVal * (1 << WeightScaleBits) * 2) = 9600 / 16384.
    // Widened to int64 for the multiply, then truncated toward zero.
    const int64_t fwd_sum = (int64_t) fc2_out[0] + ((int64_t) fc0_out[30] - (int64_t) fc0_out[31]);
    return (int32_t) (fwd_sum * (600 * 16) / (128 * 64 * 2));
}

static size_t piece_count_of(const Position *pos) {
    size_t count = 0;
    for (unsigned sq = 0; sq < SQUARE_NB; sq++) {
        if (pos->board[sq] != NO_PIECE) {
            count += 1;
        }
    }
    return count;
}

static NnueEvalOutput evaluate_bucket_raw(const Position *pos,
                                          NnueAccumulatorStack *stack,
                                          NnueRefreshCache *cache,
                                          size_t bucket) {
    alignas(CACHE_LINE_SIZE) uint8_t transformed[NNUE_TRANSFORMED_BYTES];
    NnueNnzBitset nnz;

    const NnueFeatureTransformer *ft =
      (const NnueFeatureTransformer *) (const void *) nnue_ft_ptr();
    const int32_t psqt = nnue_transform_bucket(stack, pos, ft, cache, bucket,
                                               (uint8_t) pos->side_to_move, transformed, &nnz);
    return (NnueEvalOutput) {
        .psqt = psqt,
        .positional = propagate_bucket(bucket, transformed, &nnz),
    };
}

NnueEvalOutput
nnue_inference_evaluate(const Position *pos, NnueAccumulatorStack *stack, NnueRefreshCache *cache) {
    const size_t bucket = (piece_count_of(pos) - 1) / 4;
    const NnueEvalOutput raw = evaluate_bucket_raw(pos, stack, cache, bucket);
    return (NnueEvalOutput) {
        .psqt = raw.psqt / OUTPUT_SCALE,
        .positional = raw.positional / OUTPUT_SCALE,
    };
}

NnueTraceOutput nnue_inference_trace_evaluate(const Position *pos,
                                              NnueAccumulatorStack *stack,
                                              NnueRefreshCache *cache) {
    NnueTraceOutput output = { 0 };
    output.correct_bucket = (piece_count_of(pos) - 1) / 4;

    for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS; bucket++) {
        const NnueEvalOutput raw = evaluate_bucket_raw(pos, stack, cache, bucket);
        output.psqt[bucket] = raw.psqt / OUTPUT_SCALE;
        output.positional[bucket] = raw.positional / OUTPUT_SCALE;
    }

    return output;
}
