// Reinterpret the feature-transformer weight blob as its typed regions.

#include "nnue_ft.h"

const int16_t *nnue_ft_biases(const NnueFeatureTransformer *ft) {
    const unsigned char *bytes = (const unsigned char *) ft;
    return (const int16_t *) (const void *) (bytes + NNUE_FT_BIASES_OFFSET);
}

const int16_t *nnue_ft_psq_weights(const NnueFeatureTransformer *ft) {
    const unsigned char *bytes = (const unsigned char *) ft;
    return (const int16_t *) (const void *) (bytes + NNUE_FT_PSQ_WEIGHTS_OFFSET);
}

const int8_t *nnue_ft_threat_weights(const NnueFeatureTransformer *ft) {
    const unsigned char *bytes = (const unsigned char *) ft;
    return (const int8_t *) (const void *) (bytes + NNUE_FT_THREAT_WEIGHTS_OFFSET);
}

const int32_t *nnue_ft_psq_psqt_weights(const NnueFeatureTransformer *ft) {
    const unsigned char *bytes = (const unsigned char *) ft;
    return (const int32_t *) (const void *) (bytes + NNUE_FT_PSQT_WEIGHTS_OFFSET);
}

const int32_t *nnue_ft_threat_psqt_weights(const NnueFeatureTransformer *ft) {
    const unsigned char *bytes = (const unsigned char *) ft;
    return (const int32_t *) (const void *) (bytes + NNUE_FT_THREAT_PSQT_WEIGHTS_OFFSET);
}
