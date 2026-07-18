#include "nnue_parse.h"

#include "nnue_architecture.h"
#include "nnue_common.h"

#include <string.h>

const size_t NnuePackusEpi16OrderSse41[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

// Decode COUNT signed-LEB128 values of ELEM_BYTES width from SRC into OUT.
//
// Reproduce read_leb_128_detail's integer semantics exactly. `result` is a
// uint32_t and every accumulation is unsigned, so a malformed run wraps instead
// of overflowing. The shift counter is 6 bits wide upstream: it wraps at 64, and
// the shift amount applied is that counter reduced modulo 32. Both facts matter
// only for malformed input, which is precisely the input a loaded .nnue is not
// trusted to avoid.
static bool decode_leb(const uint8_t *src,
                       size_t src_len,
                       void *out,
                       size_t elem_bytes,
                       size_t count,
                       size_t *consumed) {
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t result = 0;
        unsigned shift = 0;  // 6-bit counter; wraps at 64
        for (;;) {
            if (pos >= src_len)
                return false;
            const uint8_t byte = src[pos];
            pos += 1;
            result |= (uint32_t) (byte & 0x7f) << (shift % 32u);
            shift = (shift + 7u) & 63u;
            if ((byte & 0x80) == 0) {
                if (shift < 32u && (byte & 0x40) != 0) {
                    // Sign-extend: result | ~((1 << shift) - 1).
                    result |= ~(((uint32_t) 1 << shift) - 1u);
                }
                if (elem_bytes == 2)
                    ((int16_t *) out)[i] = (int16_t) (uint16_t) (result & 0xffffu);
                else
                    ((int32_t *) out)[i] = (int32_t) result;
                break;
            }
        }
    }
    *consumed = pos;
    return true;
}

bool nnue_decode_leb_i16(
  const uint8_t *src, size_t src_len, int16_t *out, size_t count, size_t *consumed) {
    return decode_leb(src, src_len, out, sizeof(int16_t), count, consumed);
}

bool nnue_decode_leb_i32(
  const uint8_t *src, size_t src_len, int32_t *out, size_t count, size_t *consumed) {
    return decode_leb(src, src_len, out, sizeof(int32_t), count, consumed);
}

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

// Parse one COMPRESSED_LEB128 section ([magic][u32 count][data]) of COUNT values
// of ELEM_BYTES width into OUT. Store the total section bytes consumed in
// *CONSUMED; return false when malformed.
static bool read_leb_section(const uint8_t *blob,
                             size_t blob_len,
                             void *out,
                             size_t elem_bytes,
                             size_t count,
                             size_t *consumed) {
    if (blob_len < NNUE_LEB128_MAGIC_SIZE + 4)
        return false;
    if (memcmp(blob, NNUE_LEB128_MAGIC, NNUE_LEB128_MAGIC_SIZE) != 0)
        return false;
    const size_t section_len = (size_t) nnue_read_u32_le(blob + NNUE_LEB128_MAGIC_SIZE);
    const uint8_t *data = blob + NNUE_LEB128_MAGIC_SIZE + 4;
    const size_t data_len = blob_len - (NNUE_LEB128_MAGIC_SIZE + 4);
    if (data_len < section_len)
        return false;
    // Hand the decoder the section, not the rest of the blob, so its bound is the
    // section's: the count and the section length are stated independently.
    size_t used = 0;
    if (!decode_leb(data, section_len, out, elem_bytes, count, &used))
        return false;
    if (used != section_len)
        return false;
    *consumed = NNUE_LEB128_MAGIC_SIZE + 4 + section_len;
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
    size_t used = 0;

    // Follow the read order (upstream 7c7fe322e merge): biases, threatWeights,
    // threatPsqtWeights, weights, psqtWeights -- each int32 PSQT array is its OWN
    // leb section (the base packed both into one, after weights). Storage offsets
    // are unchanged; only the stream order and framing moved.

    // 1. Read biases (LEB int16).
    if (!read_leb_section(blob + pos, blob_len - pos, (void *) (dst + NNUE_FT_BIASES_OFF),
                          sizeof(int16_t), NNUE_FT_BIASES_COUNT, &used))
        return false;
    pos += used;

    // 2. Copy threatWeights (raw int8; a byte array, so no byte order to apply).
    if (blob_len < pos + NNUE_FT_THREAT_WEIGHTS_COUNT)
        return false;
    memcpy(dst + NNUE_FT_THREAT_WEIGHTS_OFF, blob + pos, NNUE_FT_THREAT_WEIGHTS_COUNT);
    pos += NNUE_FT_THREAT_WEIGHTS_COUNT;

    // 3. Read threatPsqtWeights (LEB int32, own section).
    if (!read_leb_section(blob + pos, blob_len - pos,
                          (void *) (dst + NNUE_FT_THREAT_PSQT_WEIGHTS_OFF), sizeof(int32_t),
                          NNUE_FT_THREAT_PSQT_WEIGHTS_COUNT, &used))
        return false;
    pos += used;

    // 4. Read weights / psq weights (LEB int16).
    if (!read_leb_section(blob + pos, blob_len - pos, (void *) (dst + NNUE_FT_WEIGHTS_OFF),
                          sizeof(int16_t), NNUE_FT_PSQ_WEIGHTS_COUNT, &used))
        return false;
    pos += used;

    // 5. Read psqtWeights (LEB int32, own section).
    if (!read_leb_section(blob + pos, blob_len - pos, (void *) (dst + NNUE_FT_PSQT_WEIGHTS_OFF),
                          sizeof(int32_t), NNUE_FT_PSQT_WEIGHTS_COUNT, &used))
        return false;
    pos += used;

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
