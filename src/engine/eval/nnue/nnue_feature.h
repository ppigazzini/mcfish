// Own the three NNUE feature sets: HalfKAv2_hm (the king-bucketed piece-square
// features), full_threats (the attacker/attacked-pair features) and pp_3wide (the
// pawn-pair features), as index producers only.
//
// The INVARIANT is that a feature index is a pure function of its arguments and of the
// tables `nnue_feature_init` builds. `nnue_feature_init` MUST run before any index is
// asked for — the tables are zero, not garbage, before it, so the failure mode is a
// silent all-zero feature set rather than a crash. Call it once, at startup, in the same
// phase as `bitboards_init` / `attacks_init`.
//
// The full_threats indexer maps some (attacker, attacked) pairs OUT of range on purpose:
// `nnue_full_make_index` returns a value >= NNUE_FULL_DIMENSIONS for an excluded pair,
// and the caller drops it. That out-of-range return is the exclusion mechanism, not an
// error, and it is what upstream does.
//
// Golden: the upstream `nnue/features/half_ka_v2_hm.cpp`,
// `nnue/features/full_threats.cpp` and `nnue/features/pp_3wide.cpp`.
//
// Upstream: full_threats.h, pp_3wide.h and half_ka_v2_hm.h, the feature-set headers.

#ifndef MCFISH_NNUE_FEATURE_H
#define MCFISH_NNUE_FEATURE_H

#include "nnue_architecture.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    // 11 * 64: the per-king-bucket stride of the HalfKAv2_hm feature space.
    NNUE_PS_NB = 11 * 64,
    NNUE_FULL_DIMENSIONS = NNUE_THREAT_DIMENSIONS,
    // The first PP_3Wide index. Pair features are concatenated onto the threats, so
    // this must equal NNUE_FULL_DIMENSIONS: one index then addresses either feature
    // set's row in the single shared weight region (upstream nnue_feature_transformer.h
    // static-asserts PairFeatureSet::IndexBase == ThreatFeatureSet::Dimensions).
    NNUE_PAIR_INDEX_BASE = NNUE_THREAT_DIMENSIONS,
};

enum {
    // Bound the changed-feature lists a single ply can produce.
    NNUE_PSQ_INDEX_CAPACITY = 32,
    // Hold the threat AND pawn-pair indices: both feature sets append to one list,
    // because both index the shared threat/pair weight rows. Upstream's IndexList is
    // ValueList<u16, 256> for exactly this reason.
    NNUE_THREAT_INDEX_CAPACITY = 256,
    // Encode "no square" as 64, matching upstream's SQ_NONE in the dirty-piece record.
    NNUE_SQ_NONE = 64,
};

// Hold the per-move HalfKAv2_hm delta a make/unmake records.
//
// The FIELD ORDER IS CONTRACTUAL: `pc` first, and the struct alignment-free. The
// accumulator stores this record inside its arena at an offset that is deliberately NOT
// rounded up, and reads `pc` as the arena's first diff byte to decide whether the ply
// forces a refresh. This is the same layout as the board zone's DirtyPiece.
typedef struct NnueDirtyPiece {
    uint8_t pc;
    uint8_t from;
    uint8_t to;
    uint8_t remove_sq;
    uint8_t add_sq;
    uint8_t remove_pc;
    uint8_t add_pc;
} NnueDirtyPiece;

static_assert(sizeof(NnueDirtyPiece) == 7, "NnueDirtyPiece must be seven packed bytes");
static_assert(alignof(NnueDirtyPiece) == 1,
              "NnueDirtyPiece must stay alignment-free for the unrounded diff offset");
static_assert(offsetof(NnueDirtyPiece, pc) == 0, "the refresh test reads pc as diff byte 0");

// Hold one recorded threat delta.
//
// The bit layout, written by the board zone's make/unmake and decoded here:
//   bit 31        put_piece — the delta ADDS the threat when set, removes it when clear
//   bits 23..20   the attacking piece
//   bits 19..16   the attacked piece
//   bits 15..8    the attacked square
//   bits 7..0     the attacking square
typedef struct NnueDirtyThreatRaw {
    uint32_t data;
} NnueDirtyThreatRaw;

enum {
    NNUE_DIRTY_THREAT_PC_SQ_SHIFT = 0,
    NNUE_DIRTY_THREATENED_SQ_SHIFT = 8,
    NNUE_DIRTY_THREATENED_PC_SHIFT = 16,
    NNUE_DIRTY_THREAT_PC_SHIFT = 20,
    NNUE_DIRTY_THREAT_ADD_SHIFT = 31,
};

typedef struct NnueFullAppendResult {
    size_t len;
    uint32_t indices[NNUE_THREAT_INDEX_CAPACITY];
} NnueFullAppendResult;

// Build the feature-index tables. Call once, before any other function here.
void nnue_feature_init(void);

// --- HalfKAv2_hm ------------------------------------------------------------------

uint32_t
nnue_half_make_index(uint8_t perspective, uint8_t square, uint8_t piece, uint8_t king_square);

// Report whether DIFF moves PERSPECTIVE's own king, which invalidates every bucketed
// index and forces a full refresh.
static inline bool nnue_half_requires_refresh(NnueDirtyPiece diff, uint8_t perspective) {
    return diff.pc == (uint8_t) (6 + 8 * perspective);
}

// --- full_threats -----------------------------------------------------------------

// Return the threat feature index, or a value >= NNUE_FULL_DIMENSIONS when the
// (attacker, attacked) pair is excluded from the feature set.
uint32_t nnue_full_make_index(uint8_t perspective,
                              uint8_t attacker,
                              uint8_t from_sq,
                              uint8_t to_sq,
                              uint8_t attacked,
                              uint8_t king_square);

// Return the same index from fields already oriented — the caller has xored the square
// orientation and perspective swap in, e.g. via nnue_full_orient_mask below.
uint32_t nnue_full_make_index_oriented(uint8_t attacker_oriented,
                                       uint8_t from_oriented,
                                       uint8_t to_oriented,
                                       uint8_t attacked_oriented);

// Build the per-walk record mask: one xor of a NnueDirtyThreatRaw against it orients
// every field in place, and leaves the record's sign meaning "added" regardless of the
// walk direction. See the definition for the lane layout.
uint32_t nnue_full_orient_mask(uint8_t perspective, uint8_t king_square, bool forward);

// Append every threat feature active on BOARD (64 entries, upstream piece encoding),
// dropping the excluded pairs.
void nnue_full_append_active(uint8_t perspective,
                             uint8_t king_square,
                             const uint8_t *board,
                             const uint64_t *by_type,
                             const uint64_t *by_color,
                             NnueFullAppendResult *out);

// --- PP_3Wide ---------------------------------------------------------------------

// Hold the pawn-pair delta of one move: the two colours' pawn bitboards before and
// after it. The pair feature set is a pure function of the pawn placement, so the two
// snapshots are the whole diff — upstream's DirtyPawnPairs (types.h).
typedef struct NnueDirtyPawnPairs {
    uint64_t before[2];
    uint64_t after[2];
} NnueDirtyPawnPairs;

// Append every pawn-pair feature active on the two pawn sets. Appends onto the SAME
// list the threat features filled, so OUT->len must already hold that prefix.
void nnue_pair_append_active(NnueFullAppendResult *out,
                             uint8_t perspective,
                             uint8_t king_square,
                             uint64_t white_pawns,
                             uint64_t black_pawns);

// Append the pawn-pair delta onto the same removed/added lists the threat delta filled.
// Pairs that APPEAR go to ADDED and pairs that DISAPPEAR to REMOVED; a backward walk
// passes the two lists swapped, exactly as upstream swaps its arguments.
void nnue_pair_append_changed(uint8_t perspective,
                              uint8_t king_square,
                              const NnueDirtyPawnPairs *diff,
                              uint32_t *removed,
                              size_t *removed_len,
                              uint32_t *added,
                              size_t *added_len);

// Append the pawn-pair delta for BOTH perspectives at once, onto four lists. The pawn
// geometry the walk enumerates is perspective-independent, so a shared step pays for it
// once; each list ends up holding exactly what the per-perspective call would have put
// there. Forward walks only -- the shared step exists for the common suffix of a
// two-perspective forward catch-up.
void nnue_pair_append_changed_both(uint8_t white_king_square,
                                   uint8_t black_king_square,
                                   const NnueDirtyPawnPairs *diff,
                                   uint32_t *white_removed,
                                   size_t *white_removed_len,
                                   uint32_t *white_added,
                                   size_t *white_added_len,
                                   uint32_t *black_removed,
                                   size_t *black_removed_len,
                                   uint32_t *black_added,
                                   size_t *black_added_len);

// Report whether a king move crossing the board's centre file invalidates PERSPECTIVE's
// threat orientation. Bit 2 of the king square is the half-of-the-board bit.
static inline bool
nnue_full_requires_refresh(uint8_t us, uint8_t prev_ksq, uint8_t ksq, uint8_t perspective) {
    return perspective == us && (((int8_t) ksq & 0x4) != ((int8_t) prev_ksq & 0x4));
}

#endif  // MCFISH_NNUE_FEATURE_H
