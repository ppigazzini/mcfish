// Own the feature-transformer weight-blob layout: the byte offsets into the loaded
// `.nnue` weight arena and the typed accessors that hand back each weight region.
//
// The INVARIANT is that this header is the SOLE definition of where each region starts.
// The blob is a raw byte arena — its shape is fixed by the net file and by SIMD access,
// so the bytes stay raw — but the handle is a distinct incomplete type, not `void *`, so
// an accumulator or refresh-cache arena cannot be passed where a transformer is wanted.
// The loader must allocate it 64-byte aligned and at least `nnue_ft_blob_bytes()` long;
// every accessor's alignment rests on that.
//
// Golden: the upstream `nnue/nnue_feature_transformer.h`.

#ifndef MCFISH_NNUE_FT_H
#define MCFISH_NNUE_FT_H

#include <stddef.h>
#include <stdint.h>

#include "nnue_feature.h"

#define NNUE_ROUND_UP(value, alignment) ((((value) + (alignment) - 1) / (alignment)) * (alignment))

// NNUE_HALF_DIMENSIONS and NNUE_PSQT_BUCKETS are NOT redeclared here:
// nnue_architecture.h is the single authority for every network dimension, and
// two copies drift the day the architecture changes.
enum : size_t {
    NNUE_ALIGN = 64,

    NNUE_FT_BIASES_BYTES = NNUE_HALF_DIMENSIONS * sizeof(int16_t),
    NNUE_FT_PSQ_WEIGHTS_BYTES =
      NNUE_HALF_DIMENSIONS * NNUE_PSQ_FEATURE_DIMENSIONS * sizeof(int16_t),
    NNUE_FT_THREAT_WEIGHTS_BYTES =
      NNUE_HALF_DIMENSIONS * (size_t) NNUE_FULL_DIMENSIONS * sizeof(int8_t),
    NNUE_FT_PSQT_WEIGHTS_BYTES = NNUE_PSQ_FEATURE_DIMENSIONS * NNUE_PSQT_BUCKETS * sizeof(int32_t),
    NNUE_FT_THREAT_PSQT_WEIGHTS_BYTES =
      (size_t) NNUE_FULL_DIMENSIONS * NNUE_PSQT_BUCKETS * sizeof(int32_t),

    NNUE_FT_BIASES_OFFSET = 0,
    NNUE_FT_PSQ_WEIGHTS_OFFSET = NNUE_ROUND_UP(NNUE_FT_BIASES_BYTES, NNUE_ALIGN),
    NNUE_FT_THREAT_WEIGHTS_OFFSET =
      NNUE_ROUND_UP(NNUE_FT_PSQ_WEIGHTS_OFFSET + NNUE_FT_PSQ_WEIGHTS_BYTES, NNUE_ALIGN),
    NNUE_FT_PSQT_WEIGHTS_OFFSET =
      NNUE_ROUND_UP(NNUE_FT_THREAT_WEIGHTS_OFFSET + NNUE_FT_THREAT_WEIGHTS_BYTES, NNUE_ALIGN),
    NNUE_FT_THREAT_PSQT_WEIGHTS_OFFSET =
      NNUE_ROUND_UP(NNUE_FT_PSQT_WEIGHTS_OFFSET + NNUE_FT_PSQT_WEIGHTS_BYTES, NNUE_ALIGN),
    NNUE_FT_BLOB_BYTES = NNUE_ROUND_UP(
      NNUE_FT_THREAT_PSQT_WEIGHTS_OFFSET + NNUE_FT_THREAT_PSQT_WEIGHTS_BYTES, NNUE_ALIGN),
};

// Expose an opaque handle to the loaded weight blob. Never defined — only cast to.
typedef struct NnueFeatureTransformer NnueFeatureTransformer;

const int16_t *nnue_ft_biases(const NnueFeatureTransformer *ft);
const int16_t *nnue_ft_psq_weights(const NnueFeatureTransformer *ft);
const int8_t *nnue_ft_threat_weights(const NnueFeatureTransformer *ft);
const int32_t *nnue_ft_psq_psqt_weights(const NnueFeatureTransformer *ft);
const int32_t *nnue_ft_threat_psqt_weights(const NnueFeatureTransformer *ft);

static inline size_t nnue_ft_blob_bytes(void) { return NNUE_FT_BLOB_BYTES; }

#endif  // MCFISH_NNUE_FT_H
