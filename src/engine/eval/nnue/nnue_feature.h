// Own the two NNUE feature sets: HalfKAv2_hm (the king-bucketed piece-square features)
// and full_threats (the attacker/attacked-pair features), as index producers only.
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
// Golden: the upstream `nnue/features/half_ka_v2_hm.cpp` and
// `nnue/features/full_threats.cpp`.

#ifndef MCFISH_NNUE_FEATURE_H
#define MCFISH_NNUE_FEATURE_H

#include "nnue_architecture.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    // 11 * 64: the per-king-bucket stride of the HalfKAv2_hm feature space.
    NNUE_PS_NB = 11 * 64,
    NNUE_FULL_DIMENSIONS = 60720,
};

enum {
    // Bound the changed-feature lists a single ply can produce.
    NNUE_PSQ_INDEX_CAPACITY = 32,
    NNUE_THREAT_INDEX_CAPACITY = 128,
    // Encode "no square" as 64, matching upstream's SQ_NONE in the dirty-piece record.
    NNUE_SQ_NONE = 64,
};

// Hold the per-move HalfKAv2_hm delta a make/unmake records.
//
// The FIELD ORDER IS CONTRACTUAL: `pc` first, and the struct alignment-free. The
// accumulator stores this record inside its arena at an offset that is deliberately NOT
// rounded up, and reads `pc` as the arena's first diff byte to decide whether the ply
// forces a refresh. This is the same layout as the board zone's DirtyPiece — see
// PORT_NOTES_accumulator.md.
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

typedef struct NnueHalfAppendResult {
    size_t len;
    uint32_t indices[NNUE_PSQ_INDEX_CAPACITY];
} NnueHalfAppendResult;

typedef struct NnueFullAppendResult {
    size_t len;
    uint32_t indices[NNUE_THREAT_INDEX_CAPACITY];
} NnueFullAppendResult;

// Build the feature-index tables. Call once, before any other function here.
void nnue_feature_init(void);

// --- HalfKAv2_hm ------------------------------------------------------------------

uint32_t
nnue_half_make_index(uint8_t perspective, uint8_t square, uint8_t piece, uint8_t king_square);

// Append the indices DIFF touches, in upstream's order: `from`, then `to`, then
// `remove_sq`, then `add_sq`, each present only when its square is not NNUE_SQ_NONE.
// The caller re-derives which of them are additions and which removals from the same
// order, so the order is contractual.
void nnue_half_append_changed(uint8_t perspective,
                              uint8_t king_square,
                              NnueDirtyPiece diff,
                              NnueHalfAppendResult *out);

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

// Append one index per entry of LIST, in list order and WITHOUT the range filter, so
// index i of the result corresponds to entry i of LIST — the caller needs the pairing to
// read each entry's add/remove bit. LIST_LEN must not exceed NNUE_THREAT_INDEX_CAPACITY.
void nnue_full_append_changed(uint8_t perspective,
                              uint8_t king_square,
                              const NnueDirtyThreatRaw *list,
                              size_t list_len,
                              NnueFullAppendResult *out);

// Append every threat feature active on BOARD (64 entries, upstream piece encoding),
// dropping the excluded pairs.
void nnue_full_append_active(uint8_t perspective,
                             uint8_t king_square,
                             const uint8_t *board,
                             const uint64_t *by_type,
                             const uint64_t *by_color,
                             NnueFullAppendResult *out);

// Report whether a king move crossing the board's centre file invalidates PERSPECTIVE's
// threat orientation. Bit 2 of the king square is the half-of-the-board bit.
static inline bool
nnue_full_requires_refresh(uint8_t us, uint8_t prev_ksq, uint8_t ksq, uint8_t perspective) {
    return perspective == us && (((int8_t) ksq & 0x4) != ((int8_t) prev_ksq & 0x4));
}

#endif  // MCFISH_NNUE_FEATURE_H
