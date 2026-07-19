// Describe the SFNNv15 network shape: the transformed feature width, the eight
// layer stacks, and the three affine layers each stack holds.
//
// These dimensions are the architecture, not a tuning knob. The .nnue file
// commits to them through the architecture hash (nnue_hash.h), so a net whose
// shape differs is rejected at load rather than mis-parsed. The per-layer byte
// counts below are `sizeof` the upstream AffineTransform members and are what
// the parse and the weight storage size themselves by.
//
// Mirrors src/nnue/nnue_architecture.h:36-72.

#ifndef MCFISH_NNUE_ARCHITECTURE_H
#define MCFISH_NNUE_ARCHITECTURE_H

#include "nnue_common.h"

#include <stddef.h>

enum {
    // Number of input feature dimensions after conversion (nnue_architecture.h:43-45).
    NNUE_L1 = 1024,
    NNUE_L2 = 32,
    NNUE_L3 = 32,

    NNUE_PSQT_BUCKETS = 8,
    NNUE_LAYER_STACKS = 8,

    // NetworkArchitecture's static shape (nnue_architecture.h:58-60).
    NNUE_TRANSFORMED_FEATURE_DIMENSIONS = NNUE_L1,
    NNUE_FC_0_OUTPUTS = NNUE_L2,
    NNUE_FC_1_OUTPUTS = NNUE_L3,

    // fc_0, fc_1, fc_2. The activations carry no parameters, so only these three
    // appear in the file and in storage.
    NNUE_LAYERS_PER_STACK = 3,

    // FeatureTransformer::HalfDimensions and the two feature sets it concatenates:
    // PSQFeatureSet = half_ka_v2_hm, ThreatFeatureSet = full_threats.
    NNUE_HALF_DIMENSIONS = NNUE_L1,
    NNUE_PSQ_FEATURE_DIMENSIONS = 22528,
    NNUE_THREAT_DIMENSIONS = 60720,
};

// Name one affine layer's shape. PaddedInputDimensions is the *padded* input, the
// figure the SSSE3 weight scramble divides by, not the logical fan-in.
typedef struct {
    size_t padded_input_dimensions;
    size_t output_dimensions;
} NnueLayerDims;

// Return the descriptor for layer IDX of a stack: fc_0 1024->32, fc_1 64->32,
// fc_2 128->1. IDX must be < NNUE_LAYERS_PER_STACK.
static inline NnueLayerDims nnue_layer_dims(size_t idx) {
    static const NnueLayerDims Dims[NNUE_LAYERS_PER_STACK] = {
        { 1024, 32 },
        { 64, 32 },
        { 128, 1 },
    };
    return Dims[idx];
}

// Return sizeof(AffineTransform::biases) for layer IDX: OutputDimensions int32.
static inline size_t nnue_layer_biases_bytes(size_t idx) {
    static const size_t Bytes[NNUE_LAYERS_PER_STACK] = { 128, 128, 4 };
    return Bytes[idx];
}

// Return sizeof(AffineTransform::weights) for layer IDX: PaddedInputDimensions *
// OutputDimensions int8, stored in the SSSE3-scrambled order.
static inline size_t nnue_layer_weights_bytes(size_t idx) {
    static const size_t Bytes[NNUE_LAYERS_PER_STACK] = { 32768, 2048, 128 };
    return Bytes[idx];
}

#endif  // MCFISH_NNUE_ARCHITECTURE_H
