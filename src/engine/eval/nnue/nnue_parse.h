// Own the .nnue parse primitives and the feature-transformer memory layout.
//
// Parse the file into its already-permuted weight memory. Every routine here is
// bounds-checked against the caller's slice because the file is user input: a
// .nnue states its section length and its value count independently, so a corrupt
// one can promise more values than it carries. A malformed input must be reported,
// never trapped on.
//
// The byte offsets below are the in-memory FeatureTransformer layout, member
// order with each member alignas(64): biases, weights(psq), threatWeights,
// psqtWeights, threatPsqtWeights. The parse writes through them and inference
// reads through them, so they are the shared contract, not an implementation
// detail.
//
// Mirrors zfish nnue_parse.zig; golden src/nnue/nnue_feature_transformer.h
// (permute, read_parameters), src/nnue/layers/affine_transform.h
// (get_weight_index_scrambled), src/nnue/nnue_common.h (read_leb_128_detail).

#ifndef MCFISH_NNUE_PARSE_H
#define MCFISH_NNUE_PARSE_H

#include "nnue_architecture.h"
#include "nnue_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Count the elements of the five feature-transformer arrays.
#define NNUE_FT_BIASES_COUNT ((size_t) NNUE_HALF_DIMENSIONS)
#define NNUE_FT_PSQ_WEIGHTS_COUNT ((size_t) NNUE_HALF_DIMENSIONS * NNUE_PSQ_FEATURE_DIMENSIONS)
#define NNUE_FT_THREAT_WEIGHTS_COUNT ((size_t) NNUE_HALF_DIMENSIONS * NNUE_THREAT_DIMENSIONS)
#define NNUE_FT_PSQT_WEIGHTS_COUNT ((size_t) NNUE_PSQ_FEATURE_DIMENSIONS * NNUE_PSQT_BUCKETS)
#define NNUE_FT_THREAT_PSQT_WEIGHTS_COUNT ((size_t) NNUE_THREAT_DIMENSIONS * NNUE_PSQT_BUCKETS)

// Lay out the in-memory byte offsets. Each is a multiple of the cache line, which
// is what lets the typed views below be formed by casting the base pointer.
#define NNUE_FT_BIASES_OFF ((size_t) 0)
#define NNUE_FT_WEIGHTS_OFF \
    NNUE_CEIL_TO_MULTIPLE(NNUE_FT_BIASES_COUNT * 2, (size_t) NNUE_CACHE_LINE_SIZE)
#define NNUE_FT_THREAT_WEIGHTS_OFF \
    NNUE_CEIL_TO_MULTIPLE(NNUE_FT_WEIGHTS_OFF + NNUE_FT_PSQ_WEIGHTS_COUNT * 2, \
                          (size_t) NNUE_CACHE_LINE_SIZE)
#define NNUE_FT_PSQT_WEIGHTS_OFF \
    NNUE_CEIL_TO_MULTIPLE(NNUE_FT_THREAT_WEIGHTS_OFF + NNUE_FT_THREAT_WEIGHTS_COUNT * 1, \
                          (size_t) NNUE_CACHE_LINE_SIZE)
#define NNUE_FT_THREAT_PSQT_WEIGHTS_OFF \
    NNUE_CEIL_TO_MULTIPLE(NNUE_FT_PSQT_WEIGHTS_OFF + NNUE_FT_PSQT_WEIGHTS_COUNT * 4, \
                          (size_t) NNUE_CACHE_LINE_SIZE)
#define NNUE_FT_TOTAL_BYTES \
    NNUE_CEIL_TO_MULTIPLE(NNUE_FT_THREAT_PSQT_WEIGHTS_OFF + NNUE_FT_THREAT_PSQT_WEIGHTS_COUNT * 4, \
                          (size_t) NNUE_CACHE_LINE_SIZE)

// Define the SSE4.1 PackusEpi16Order as the identity. Keep it explicit so the
// assumption is visible and a future wide-SIMD target can swap it.
extern const size_t NnuePackusEpi16OrderSse41[8];

// Decode COUNT signed-LEB128 values from SRC into OUT, storing the number of
// source bytes consumed in *CONSUMED. Return false when SRC runs out first.
//
// Mirrors read_leb_128_detail: 7 bits per byte, the shift masked to 32, and
// sign-extension when the final shift is < 32 and bit 0x40 is set. The shift
// counter itself is 6 bits wide and wraps, which is why it is masked to 64 here
// before being reduced modulo 32 — a wider counter would stop aliasing and
// change what a long malformed run decodes to.
bool nnue_decode_leb_i16(
  const uint8_t *src, size_t src_len, int16_t *out, size_t count, size_t *consumed);
bool nnue_decode_leb_i32(
  const uint8_t *src, size_t src_len, int32_t *out, size_t count, size_t *consumed);

// Reorder blocks per permute<BlockSize>: ORDER_LEN blocks of BLOCK_SIZE bytes
// within each (BLOCK_SIZE * ORDER_LEN)-byte chunk of DATA. SCRATCH must hold at
// least one chunk. DATA_LEN must be a whole number of chunks.
void nnue_permute_blocks(uint8_t *data,
                         size_t data_len,
                         size_t block_size,
                         const size_t *order,
                         size_t order_len,
                         uint8_t *scratch);

// Compute get_weight_index_scrambled(I): the SSSE3 affine weight index
// permutation the layer parse writes through and propagate reads back.
static inline size_t
nnue_weight_index_scrambled(size_t i, size_t padded_input, size_t output_dims) {
    return (i / 4) % (padded_input / 4) * output_dims * 4 + i / padded_input * 4 + i % 4;
}

// Parse the feature-transformer blob into DST (NNUE_FT_TOTAL_BYTES of 64-byte
// aligned storage) and store the blob bytes consumed in *CONSUMED. Return false
// on malformed input. No permute: PackusEpi16Order is the identity on this target.
bool nnue_parse_feature_transformer(const uint8_t *blob,
                                    size_t blob_len,
                                    uint8_t *dst,
                                    size_t *consumed);

// Parse one affine layer at the start of BLOB: biases (OutputDimensions int32,
// little-endian, linear) then weights (int8, written through the SSSE3 scramble).
// OutputDimensions and PaddedInputDimensions are derived from the destination
// sizes. Store the bytes consumed in *CONSUMED; return false on malformed input.
bool nnue_parse_layer(const uint8_t *blob,
                      size_t blob_len,
                      uint8_t *biases_dst,
                      size_t biases_len,
                      uint8_t *weights_dst,
                      size_t weights_len,
                      size_t *consumed);

#endif  // MCFISH_NNUE_PARSE_H
