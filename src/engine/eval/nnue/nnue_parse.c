#include "nnue_parse.h"

#include "nnue_architecture.h"
#include "nnue_common.h"

#include <string.h>

const size_t NnuePackusEpi16OrderSse41[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

void nnue_permute_blocks(uint8_t *data,
                         size_t data_len,
                         size_t block_size,
                         const size_t *order,
                         size_t order_len,
                         uint8_t *scratch) {
    const size_t chunk = block_size * order_len;
    if (chunk == 0 || data_len % chunk != 0)
        return;
    for (size_t i = 0; i < data_len; i += chunk) {
        uint8_t *values = data + i;
        for (size_t j = 0; j < order_len; ++j)
            memcpy(scratch + j * block_size, values + order[j] * block_size, block_size);
        memcpy(values, scratch, chunk);
    }
}

// Read COUNT little-endian int16 values from BLOB into OUT, the plain section
// upstream's read_little_endian reads: no framing, no length of its own, so the only
// bound is the caller's slice. Convert rather than copy, since the storage holds host
// order and the file holds little-endian.
static bool read_i16_section(const uint8_t *blob, size_t blob_len, int16_t *out, size_t count) {
    if (blob_len < count * sizeof(int16_t))
        return false;
    for (size_t i = 0; i < count; ++i)
        out[i] = nnue_read_i16_le(blob + i * sizeof(int16_t));
    return true;
}

bool nnue_parse_feature_transformer(const uint8_t *blob,
                                    size_t blob_len,
                                    uint8_t *dst,
                                    size_t *consumed) {
    // Skip the leading uint32_t component hash (Detail::read_parameters). Check it
    // is there first: a file shorter than the hash would make the very first
    // section slice run out of range.
    if (blob_len < 4)
        return false;
    size_t pos = 4;

    // Follow the SFNNv18 read order: biases, threatWeights, ppWeights, weights. Every
    // section is plain little-endian, the int8 ones a byte array with no order to
    // apply. The threat and pp weight sections are framed separately in the stream
    // but land in one CONTIGUOUS threat region, pp right after threat, so one index
    // addresses either feature set's row at runtime.

    // 1. Read biases (int16).
    if (!read_i16_section(blob + pos, blob_len - pos,
                          (int16_t *) (void *) (dst + NNUE_FT_BIASES_OFF), NNUE_FT_BIASES_COUNT))
        return false;
    pos += NNUE_FT_BIASES_COUNT * sizeof(int16_t);

    // 2. Copy threatWeights (int8).
    if (blob_len < pos + NNUE_FT_THREAT_ONLY_WEIGHTS_COUNT)
        return false;
    memcpy(dst + NNUE_FT_THREAT_WEIGHTS_OFF, blob + pos, NNUE_FT_THREAT_ONLY_WEIGHTS_COUNT);
    pos += NNUE_FT_THREAT_ONLY_WEIGHTS_COUNT;

    // 3. Copy ppWeights (int8) onto the tail of the threat weight region.
    if (blob_len < pos + NNUE_FT_PAIR_ONLY_WEIGHTS_COUNT)
        return false;
    memcpy(dst + NNUE_FT_PAIR_WEIGHTS_OFF, blob + pos, NNUE_FT_PAIR_ONLY_WEIGHTS_COUNT);
    pos += NNUE_FT_PAIR_ONLY_WEIGHTS_COUNT;

    // 4. Copy weights / psq weights (int8).
    if (blob_len < pos + NNUE_FT_PSQ_WEIGHTS_COUNT)
        return false;
    memcpy(dst + NNUE_FT_WEIGHTS_OFF, blob + pos, NNUE_FT_PSQ_WEIGHTS_COUNT);
    pos += NNUE_FT_PSQ_WEIGHTS_COUNT;

    *consumed = pos;
    return true;
}

bool nnue_parse_layer(const uint8_t *blob,
                      size_t blob_len,
                      uint8_t *biases_dst,
                      size_t biases_len,
                      uint8_t *weights_dst,
                      size_t weights_len,
                      size_t *consumed) {
    const size_t output_dims = biases_len / sizeof(int32_t);
    if (output_dims == 0)
        return false;
    if (blob_len < biases_len + weights_len)
        return false;

    // Read the biases little-endian into native int32: inference reads them as
    // int32, so the storage must hold host order, not file order.
    int32_t *biases = (int32_t *) (void *) biases_dst;
    for (size_t j = 0; j < output_dims; ++j)
        biases[j] = nnue_read_i32_le(blob + j * sizeof(int32_t));

    size_t pos = biases_len;
    const size_t n = weights_len;  // int8 weights
    const size_t padded_input = n / output_dims;
    for (size_t i = 0; i < n; ++i)
        weights_dst[nnue_weight_index_scrambled(i, padded_input, output_dims)] = blob[pos + i];
    pos += n;

    *consumed = pos;
    return true;
}
