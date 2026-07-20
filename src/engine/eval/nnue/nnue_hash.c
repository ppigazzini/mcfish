#include "nnue_hash.h"

#include "nnue_architecture.h"
#include "nnue_common.h"
#include "nnue_parse.h"

#include <stdbool.h>

#define NNUE_HASH_COMBINE_MAGIC ((size_t) 0x9e3779b9)

// AffineTransform and Clipped/SqrClippedReLU get_hash_value bases.
#define NNUE_AFFINE_BASE 0xCC03DAE4u
#define NNUE_CLIPPED_BASE 0x538D24C7u

// Read a little-endian uint64_t from P: the hash is defined over the file's byte
// order, so assemble it rather than casting.
static uint64_t read_u64_le(const uint8_t *p) {
    return (uint64_t) p[0] | ((uint64_t) p[1] << 8) | ((uint64_t) p[2] << 16)
         | ((uint64_t) p[3] << 24) | ((uint64_t) p[4] << 32) | ((uint64_t) p[5] << 40)
         | ((uint64_t) p[6] << 48) | ((uint64_t) p[7] << 56);
}

uint64_t nnue_hash_bytes(const uint8_t *data, size_t len) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const unsigned r = 47;
    uint64_t h = (uint64_t) len * m;

    const size_t tail = len & ~(size_t) 7;
    for (size_t p = 0; p != tail; p += 8) {
        uint64_t k = read_u64_le(data + p);
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }

    if ((len & 7) != 0) {
        uint64_t k = 0;
        size_t i = len & 7;
        while (i > 0) {
            i -= 1;
            // SIGN-extend. Upstream's hash_bytes takes `const char*` and does
            // `u64(end[i])` (misc.cpp:481); plain char is SIGNED on x86-64, so a
            // tail byte >= 0x80 fills the high bits with ones. Zero-extending
            // here would silently produce a different digest for any input whose
            // length is not a multiple of 8 and whose tail has the high bit set.
            k = (k << 8) | (uint64_t) (int64_t) (int8_t) data[tail + i];
        }
        h ^= k;
        h *= m;
    }

    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

void nnue_hash_combine(size_t *seed, size_t v) {
    *seed ^= v + NNUE_HASH_COMBINE_MAGIC + (*seed << 6) + (*seed >> 2);
}

static void raw_data_hash(size_t *seed, const uint8_t *bytes, size_t len) {
    nnue_hash_combine(seed, (size_t) nnue_hash_bytes(bytes, len));
}

// ---- feature transformer -----------------------------------------------------

// Implement combine_hash (nnue_feature_transformer.h): rotate-left-1 then xor.
static uint32_t combine_hash(const uint32_t *hashes, size_t count) {
    uint32_t hash = 0;
    for (size_t i = 0; i < count; ++i) {
        hash = (hash << 1) | (hash >> 31);
        hash ^= hashes[i];
    }
    return hash;
}

uint32_t nnue_feature_transformer_hash_value(void) {
    // ThreatFeatureSet = full_threats (0x8f234cb8), PSQFeatureSet = half_ka_v2_hm
    // (0x7f234cb8); OutputDimensions = HalfDimensions = 1024.
    static const uint32_t FeatureSetHashes[2] = { 0x8f234cb8u, 0x7f234cb8u };
    return combine_hash(FeatureSetHashes, 2) ^ ((uint32_t) NNUE_HALF_DIMENSIONS * 2u);
}

size_t nnue_feature_transformer_content_hash(const uint8_t *ft) {
    size_t h = 0;
    raw_data_hash(&h, ft + NNUE_FT_BIASES_OFF, NNUE_FT_BIASES_COUNT * 2);
    raw_data_hash(&h, ft + NNUE_FT_WEIGHTS_OFF, NNUE_FT_PSQ_WEIGHTS_COUNT * 2);
    raw_data_hash(&h, ft + NNUE_FT_PSQT_WEIGHTS_OFF, NNUE_FT_PSQT_WEIGHTS_COUNT * 4);
    raw_data_hash(&h, ft + NNUE_FT_THREAT_WEIGHTS_OFF, NNUE_FT_THREAT_WEIGHTS_COUNT);
    raw_data_hash(&h, ft + NNUE_FT_THREAT_PSQT_WEIGHTS_OFF, NNUE_FT_THREAT_PSQT_WEIGHTS_COUNT * 4);
    nnue_hash_combine(&h, nnue_feature_transformer_hash_value());
    return h;
}

// ---- layer stack -------------------------------------------------------------

// Compute AffineTransform/AffineTransformSparseInput::get_hash_value(prevHash).
static uint32_t affine_hash_value(uint32_t prev, uint32_t out_dims) {
    uint32_t hv = NNUE_AFFINE_BASE + out_dims;
    hv ^= prev >> 1;
    hv ^= prev << 31;
    return hv;
}

// Compute AffineTransform::get_content_hash: hash biases, weights, then
// get_hash_value(0) — prevHash is 0, so both xors vanish.
static size_t affine_content_hash(const uint8_t *biases,
                                  size_t biases_len,
                                  const uint8_t *weights,
                                  size_t weights_len,
                                  uint32_t out_dims) {
    size_t h = 0;
    raw_data_hash(&h, biases, biases_len);
    raw_data_hash(&h, weights, weights_len);
    nnue_hash_combine(&h, NNUE_AFFINE_BASE + out_dims);
    return h;
}

// Compute ClippedReLU/SqrClippedReLU::get_content_hash: get_hash_value(0). The
// activations carry no parameters, so this is a constant.
static size_t activation_content_hash(void) {
    size_t h = 0;
    nnue_hash_combine(&h, NNUE_CLIPPED_BASE);
    return h;
}

uint32_t nnue_architecture_hash_value(void) {
    uint32_t hv = 0xEC42E90Du;
    hv ^= (uint32_t) NNUE_HALF_DIMENSIONS * 2u;  // TransformedFeatureDimensions * 2
    hv = affine_hash_value(hv, 32);              // fc_0, OutputDimensions = FC_0_OUTPUTS
    hv = NNUE_CLIPPED_BASE + hv;                 // ac_0
    hv = affine_hash_value(hv, 32);              // fc_1, FC_1_OUTPUTS
    hv = NNUE_CLIPPED_BASE + hv;                 // ac_1
    hv = affine_hash_value(hv, 1);               // fc_2
    return hv;
}

size_t nnue_layer_stack_content_hash(const uint8_t *fc0_biases,
                                     size_t fc0_biases_len,
                                     const uint8_t *fc0_weights,
                                     size_t fc0_weights_len,
                                     const uint8_t *fc1_biases,
                                     size_t fc1_biases_len,
                                     const uint8_t *fc1_weights,
                                     size_t fc1_weights_len,
                                     const uint8_t *fc2_biases,
                                     size_t fc2_biases_len,
                                     const uint8_t *fc2_weights,
                                     size_t fc2_weights_len) {
    size_t h = 0;
    nnue_hash_combine(
      &h, affine_content_hash(fc0_biases, fc0_biases_len, fc0_weights, fc0_weights_len, 32));
    nnue_hash_combine(&h, activation_content_hash());  // ac_sqr_0
    nnue_hash_combine(&h, activation_content_hash());  // ac_0
    nnue_hash_combine(
      &h, affine_content_hash(fc1_biases, fc1_biases_len, fc1_weights, fc1_weights_len, 32));
    nnue_hash_combine(&h, activation_content_hash());  // ac_1
    nnue_hash_combine(
      &h, affine_content_hash(fc2_biases, fc2_biases_len, fc2_weights, fc2_weights_len, 1));
    nnue_hash_combine(&h, nnue_architecture_hash_value());
    return h;
}

uint32_t nnue_network_hash_value(void) {
    return nnue_feature_transformer_hash_value() ^ nnue_architecture_hash_value();
}

size_t nnue_eval_file_content_hash(const char *default_name,
                                   size_t default_name_len,
                                   const char *current,
                                   size_t current_len,
                                   const char *description,
                                   size_t description_len) {
    size_t h = 0;
    nnue_hash_combine(&h,
                      (size_t) nnue_hash_bytes((const uint8_t *) default_name, default_name_len));
    nnue_hash_combine(&h, (size_t) nnue_hash_bytes((const uint8_t *) current, current_len));
    nnue_hash_combine(&h, (size_t) nnue_hash_bytes((const uint8_t *) description, description_len));
    return h;
}
