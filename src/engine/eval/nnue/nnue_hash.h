// Own the NNUE hash computations: the architecture hashes the file header commits
// to, and the content hashes of the loaded weights.
//
// Two families live here and must not be confused. The *hash values* are
// compile-time functions of the architecture alone — the network hash value is
// what a .nnue header carries, so a net built for a different shape is rejected
// at load. The *content hashes* are MurmurHash2-64A over the loaded weight bytes,
// combined in upstream's member order; they identify a particular net, not a
// shape.
//
// Every arithmetic step here is unsigned and wrapping by design; the rotate in
// combine_hash and the 6/2 shifts in hash_combine are part of the definition.
//
// Mirrors zfish nnue_hash.zig; golden src/nnue/network.cpp (get_content_hash),
// src/nnue/nnue_architecture.h (get_hash_value), src/misc.cpp (hash_bytes).

#ifndef MCFISH_NNUE_HASH_H
#define MCFISH_NNUE_HASH_H

#include <stddef.h>
#include <stdint.h>

// Implement hash_bytes: MurmurHash2 64-bit (misc.cpp).
uint64_t nnue_hash_bytes(const uint8_t *data, size_t len);

// Implement hash_combine for an integral value (misc.h):
// seed ^= v + 0x9e3779b9 + (seed << 6) + (seed >> 2).
void nnue_hash_combine(size_t *seed, size_t v);

// Compute FeatureTransformer::get_hash_value.
uint32_t nnue_feature_transformer_hash_value(void);

// Compute NetworkArchitecture::get_hash_value (nnue_architecture.h), the chained
// variant that threads prevHash through fc_0, ac_0, fc_1, ac_1, fc_2.
uint32_t nnue_architecture_hash_value(void);

// Compute Network::hash (network.h): the evaluation-function structure hash
// embedded in the file header. This is the value a load compares against.
uint32_t nnue_network_hash_value(void);

// Compute FeatureTransformer::get_content_hash over the resident weight image FT
// (NNUE_FT_TOTAL_BYTES). The raw-data hashes run in member-value order: biases,
// weights, psqtWeights, threatWeights, threatPsqtWeights.
size_t nnue_feature_transformer_content_hash(const uint8_t *ft);

// Compute NetworkArchitecture::get_content_hash for one layer stack.
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
                                     size_t fc2_weights_len);

// Compute the eval-file identity hash: combine the byte hashes of the default
// name, the current name, and the net description.
size_t nnue_eval_file_content_hash(const char *default_name,
                                   size_t default_name_len,
                                   const char *current,
                                   size_t current_len,
                                   const char *description,
                                   size_t description_len);

#endif  // MCFISH_NNUE_HASH_H
