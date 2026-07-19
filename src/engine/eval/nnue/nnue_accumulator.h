// Own the incremental NNUE accumulator: the per-ply accumulator stack, the
// per-(king-square, perspective) refresh cache, and the transform that turns the latest
// accumulator into the first layer's int8 input.
//
// THE INVARIANT IS THAT THE ACCUMULATOR IS INCREMENTAL. Slot `i` of the stack holds the
// accumulator after `i` plies AND the diff that produced it; `nnue_acc_evaluate` walks
// from the nearest computed slot to the top, applying diffs, and only falls back to a
// refresh when a king move invalidated the bucketed features. So the diffs are not
// optional bookkeeping: EVERY make/unmake must push and pop, and every make must fill the
// records `nnue_acc_stack_push` hands back, or the accumulator silently describes a
// different position than the board does. See PORT_NOTES_accumulator.md for the exact
// Position hooks this needs.
//
// Both arenas are raw byte buffers the CALLER owns and allocates, 64-byte aligned, sized
// by `nnue_accumulator_stack_bytes` / `nnue_refresh_cache_bytes`. They are opaque
// incomplete types rather than `void *` so the three NNUE arenas cannot be confused.
//
// Ported from zfish `engine/eval/nnue_accumulator.zig`, `nnue_acc_layout.zig`,
// `nnue_acc_rowops.zig`, `nnue_acc_update.zig` and `nnue_refresh_cache.zig` against the
// upstream golden `nnue/nnue_accumulator.cpp`.

#ifndef MCFISH_NNUE_ACCUMULATOR_H
#define MCFISH_NNUE_ACCUMULATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../board/position.h"
#include "nnue_feature.h"
#include "nnue_ft.h"

enum {
    // Bound the stack at MAX_PLY + 1 slots: one root plus one per ply.
    NNUE_MAX_STACK_SIZE = 247,
    NNUE_DIRTY_THREAT_CAPACITY = 96,
    NNUE_COLOR_COUNT = 2,
    // Set one bit per 4-byte chunk of the transformed output.
    NNUE_NNZ_WORD_COUNT = NNUE_HALF_DIMENSIONS * 2 / 4 / 64,
    NNUE_TRANSFORMED_BYTES = NNUE_HALF_DIMENSIONS,
};

// Hold the per-move threat deltas one ply records. Layout-identical to the board zone's
// DirtyThreats — see PORT_NOTES_accumulator.md.
typedef struct NnueDirtyThreatList {
    NnueDirtyThreatRaw values[NNUE_DIRTY_THREAT_CAPACITY];
    size_t size;
} NnueDirtyThreatList;

typedef struct NnueDirtyThreats {
    NnueDirtyThreatList list;
    uint8_t us;
    uint8_t prev_ksq;
    uint8_t ksq;
} NnueDirtyThreats;

// Record which 4-byte chunks of the transformed output are non-zero, so the first affine
// layer can skip the rest. Written by the transform, which has the values in a register
// already; read by nnue_affine.
typedef uint64_t NnueNnzBitset[NNUE_NNZ_WORD_COUNT];

// Expose opaque handles to the two caller-owned arenas.
typedef struct NnueAccumulatorStack NnueAccumulatorStack;
typedef struct NnueRefreshCache NnueRefreshCache;

// Return the byte size each arena must have. Both need NNUE_ALIGN alignment.
size_t nnue_accumulator_stack_bytes(void);
size_t nnue_refresh_cache_bytes(void);

// Seed every refresh entry with the empty board: accumulation = the transformer biases,
// everything else zero. Call after loading a net and before the first evaluate.
void nnue_clear_refresh_cache(NnueRefreshCache *cache, const int16_t *biases);

// Hand back the two records the make-move must fill for the ply just pushed.
typedef struct NnueStackPushOutput {
    NnueDirtyPiece *dirty_piece;
    NnueDirtyThreats *dirty_threats;
} NnueStackPushOutput;

// Reset the stack to one uncomputed root slot with a zeroed diff.
void nnue_acc_stack_reset(NnueAccumulatorStack *stack);

// Push a fresh uncomputed slot and return the ply's diff records. The caller MUST fill
// both before the next evaluate. Pushing past NNUE_MAX_STACK_SIZE is a caller error.
NnueStackPushOutput nnue_acc_stack_push(NnueAccumulatorStack *stack);

void nnue_acc_stack_pop(NnueAccumulatorStack *stack);

// Bring both perspectives of the top slot up to date for POS.
void nnue_acc_evaluate(NnueAccumulatorStack *stack,
                       const Position *pos,
                       const NnueFeatureTransformer *ft,
                       NnueRefreshCache *cache);

// Bring the accumulator up to date, then write the layer-0 input.
//
// OUTPUT receives NNUE_TRANSFORMED_BYTES bytes and must be NNUE_ALIGN aligned; NNZ
// receives the non-zero-chunk bitset. Return the psqt term for BUCKET, from STM's point
// of view.
int32_t nnue_transform_bucket(NnueAccumulatorStack *stack,
                              const Position *pos,
                              const NnueFeatureTransformer *ft,
                              NnueRefreshCache *cache,
                              size_t bucket,
                              uint8_t stm,
                              uint8_t *output,
                              NnueNnzBitset *nnz);

#endif  // MCFISH_NNUE_ACCUMULATOR_H
