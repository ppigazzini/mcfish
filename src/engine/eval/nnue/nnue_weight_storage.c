// Define _GNU_SOURCE before any header: madvise and MADV_HUGEPAGE are GNU
// extensions, and without it the constant is undefined and the huge-page hint
// below compiles away into a fallback that looks clean and does nothing.
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include "nnue_weight_storage.h"

#include "nnue_architecture.h"
#include "nnue_common.h"

#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
    #include <sys/mman.h>
#endif

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

// Return N uninitialised bytes aligned to the cache line, or NULL. Round the
// request up to the alignment: aligned_alloc wants a size the alignment divides.
//
// Do not zero the block. A successful parse writes every byte a reader can
// reach: the five feature-transformer sections butt-join on 64-byte multiples
// with no padding between them, the layer biases are written linearly, and the
// layer weight scramble is a bijection on [0, n). Zeroing here would fault in
// and dirty the whole ~106 MB arena once just for the parse to overwrite every
// byte of it again — pure startup dead work. A FAILED parse leaves unwritten
// garbage, exactly as it left unread zeroes before: the loaded-state name is
// not set, so the verify gate refuses to search either way.
// Ask for transparent huge pages on blocks large enough to be worth one. The
// threat feature weights alone are ~62 MB and are indexed by a scattered feature
// row on every accumulator update, so at 4 KiB pages the walk spans roughly
// fifteen thousand TLB entries. The hint is advisory -- the kernel may decline,
// and it is a no-op where MADV_HUGEPAGE does not exist -- so it can only change
// performance, never a value. Alignment must be raised to the huge-page boundary
// for the kernel to be able to honour it at all.
//
// NOT VERIFIED ON THIS HOST. The WSL2 kernel here returns success from madvise and
// then backs nothing: AnonHugePages reads 0 for this process and for every other
// process on the system, with THP set to [madvise]. So the benefit is unmeasured,
// not measured-and-zero, and no throughput claim rests on it. It is here because
// upstream makes the call and because [madvise] means a process that
// does NOT ask is guaranteed to get nothing.
enum { HUGE_PAGE_SIZE = 2u << 20, HUGE_PAGE_MIN_BLOCK = 1u << 20 };

static uint8_t *aligned_uninit(size_t n) {
    if (n == 0)
        return NULL;

    const bool huge = n >= HUGE_PAGE_MIN_BLOCK;
    const size_t align = huge ? (size_t) HUGE_PAGE_SIZE : (size_t) NNUE_CACHE_LINE_SIZE;
    const size_t total = NNUE_CEIL_TO_MULTIPLE(n, align);
    uint8_t *block = aligned_alloc(align, total);
    if (block == NULL)
        return NULL;

#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (huge)
        (void) madvise(block, total, MADV_HUGEPAGE);
#endif

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
        FtStorage = aligned_uninit(n);
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
        *slot = aligned_uninit(n);
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
