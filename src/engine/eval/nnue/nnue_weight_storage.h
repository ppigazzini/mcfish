// Own the NNUE weight storage and the loaded-net identity.
//
// Split out of network.c so the inference path and the file-I/O path share one
// owner without importing each other: the parse writes the weights straight into
// these buffers and inference reads from the same memory, and owning them here
// keeps that split acyclic.
//
// The feature transformer is ~106 MiB of SIMD-permuted weights; each per-bucket
// affine stack holds fc_0/fc_1/fc_2 biases and weights. Every block is zeroed and
// at least 64-byte aligned, which is what lets the parse form typed views by
// casting the base pointer.
//
// Golden: src/nnue/network.cpp.

#ifndef MCFISH_NNUE_WEIGHT_STORAGE_H
#define MCFISH_NNUE_WEIGHT_STORAGE_H

#include "nnue_architecture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Name the two arrays an affine layer stores, so a call site reads as
// NNUE_LAYER_BIASES rather than 0.
typedef enum : uint8_t {
    NNUE_LAYER_BIASES = 0,
    NNUE_LAYER_WEIGHTS = 1,
} NnueLayerPart;

// ---- loaded-net identity -----------------------------------------------------

void nnue_mark_initialized(void);
bool nnue_is_initialized(void);

// Record the name and description of the net just loaded. Both are truncated to
// the fixed 256-byte buffers, as upstream's fixed net-identity fields are.
void nnue_set_loaded_state(const char *current,
                           size_t current_len,
                           const char *description,
                           size_t description_len);

// Return the loaded net's name / description. Both are NUL-terminated for
// printing; *LEN, when non-NULL, receives the byte length, which is what a
// comparison must use — a description may contain embedded NULs.
const char *nnue_nn_current(size_t *len);
const char *nnue_nn_description(size_t *len);

// Report whether the loaded net's name equals TARGET.
bool nnue_equal_current_name(const char *target, size_t target_len);

// ---- weight storage ----------------------------------------------------------

// Return the feature-transformer block, allocating (or re-allocating on a size
// change) N zeroed, 64-byte-aligned bytes. Return NULL when N is 0 or the
// allocation fails; the caller must then reject the net rather than proceed.
uint8_t *nnue_ft_storage(size_t n);

// Return the loaded feature transformer, or NULL when no net is resident.
const uint8_t *nnue_ft_ptr(void);

// Return one affine layer's block, allocating N zeroed, 64-byte-aligned bytes on
// first use. Return NULL on a bad index or a failed allocation.
uint8_t *nnue_layer_storage(size_t bucket, size_t idx, NnueLayerPart part, size_t n);

// Return one affine layer's block, or NULL when it has never been loaded.
const uint8_t *nnue_layer_ptr(size_t bucket, size_t idx, NnueLayerPart part);

// Release every block and forget the loaded-net identity. A subsequent load
// re-allocates.
void nnue_weight_storage_free(void);

#endif  // MCFISH_NNUE_WEIGHT_STORAGE_H
