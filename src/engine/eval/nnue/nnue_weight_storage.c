#include "nnue_weight_storage.h"

#include "nnue_architecture.h"
#include "nnue_common.h"

#include <stdlib.h>
#include <string.h>

#define NNUE_NAME_MAX 256

// Track the loaded-net identity: the current EvalFile name, its description, and
// the initialized flag. The load path owns these.
static bool NnInitialized = false;
static char NnCurrent[NNUE_NAME_MAX + 1];
static size_t NnCurrentLen = 0;
static char NnDescription[NNUE_NAME_MAX + 1];
static size_t NnDescriptionLen = 0;

// Hold the inference storage. The parse writes the weights straight here;
// inference reads from the same memory.
static uint8_t *FtStorage = NULL;
static size_t FtLen = 0;

static uint8_t *LayerWeights[NNUE_LAYER_STACKS][NNUE_LAYERS_PER_STACK];
static uint8_t *LayerBiases[NNUE_LAYER_STACKS][NNUE_LAYERS_PER_STACK];

// Return N zeroed bytes aligned to the cache line, or NULL. Round the request up
// to the alignment: aligned_alloc wants a size the alignment divides, and the
// slack costs at most 63 bytes on blocks measured in megabytes.
static uint8_t *aligned_zeroed(size_t n) {
    if (n == 0)
        return NULL;
    const size_t total = NNUE_CEIL_TO_MULTIPLE(n, (size_t) NNUE_CACHE_LINE_SIZE);
    uint8_t *block = aligned_alloc(NNUE_CACHE_LINE_SIZE, total);
    if (block == NULL)
        return NULL;
    memset(block, 0, total);
    return block;
}

void nnue_mark_initialized(void) { NnInitialized = true; }

bool nnue_is_initialized(void) { return NnInitialized; }

void nnue_set_loaded_state(const char *current,
                           size_t current_len,
                           const char *description,
                           size_t description_len) {
    const size_t cl = current_len < NNUE_NAME_MAX ? current_len : NNUE_NAME_MAX;
    memcpy(NnCurrent, current, cl);
    NnCurrent[cl] = '\0';
    NnCurrentLen = cl;

    const size_t dl = description_len < NNUE_NAME_MAX ? description_len : NNUE_NAME_MAX;
    memcpy(NnDescription, description, dl);
    NnDescription[dl] = '\0';
    NnDescriptionLen = dl;
}

const char *nnue_nn_current(size_t *len) {
    if (len != NULL)
        *len = NnCurrentLen;
    return NnCurrent;
}

const char *nnue_nn_description(size_t *len) {
    if (len != NULL)
        *len = NnDescriptionLen;
    return NnDescription;
}

bool nnue_equal_current_name(const char *target, size_t target_len) {
    return target_len == NnCurrentLen && memcmp(NnCurrent, target, target_len) == 0;
}

uint8_t *nnue_ft_storage(size_t n) {
    if (n == 0)
        return NULL;
    if (FtStorage != NULL && FtLen != n) {
        free(FtStorage);
        FtStorage = NULL;
        FtLen = 0;
    }
    if (FtStorage == NULL) {
        FtStorage = aligned_zeroed(n);
        if (FtStorage == NULL)
            return NULL;
        FtLen = n;
    }
    return FtStorage;
}

const uint8_t *nnue_ft_ptr(void) { return FtStorage; }

static uint8_t **layer_slot(size_t bucket, size_t idx, NnueLayerPart part) {
    if (bucket >= NNUE_LAYER_STACKS || idx >= NNUE_LAYERS_PER_STACK)
        return NULL;
    return part == NNUE_LAYER_WEIGHTS ? &LayerWeights[bucket][idx] : &LayerBiases[bucket][idx];
}

uint8_t *nnue_layer_storage(size_t bucket, size_t idx, NnueLayerPart part, size_t n) {
    uint8_t **slot = layer_slot(bucket, idx, part);
    if (slot == NULL || n == 0)
        return NULL;
    if (*slot == NULL)
        *slot = aligned_zeroed(n);
    return *slot;
}

const uint8_t *nnue_layer_ptr(size_t bucket, size_t idx, NnueLayerPart part) {
    uint8_t **slot = layer_slot(bucket, idx, part);
    return slot == NULL ? NULL : *slot;
}

void nnue_weight_storage_free(void) {
    free(FtStorage);
    FtStorage = NULL;
    FtLen = 0;
    for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS; ++bucket) {
        for (size_t idx = 0; idx < NNUE_LAYERS_PER_STACK; ++idx) {
            free(LayerWeights[bucket][idx]);
            LayerWeights[bucket][idx] = NULL;
            free(LayerBiases[bucket][idx]);
            LayerBiases[bucket][idx] = NULL;
        }
    }
    NnInitialized = false;
    NnCurrent[0] = '\0';
    NnCurrentLen = 0;
    NnDescription[0] = '\0';
    NnDescriptionLen = 0;
}
