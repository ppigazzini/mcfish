// Own the Syzygy compressed-stream half: parse one PairsData header out of a
// mapped file (`decode_set_sizes`) and decompress the value stored at an index
// (`decode_pairs`).
//
// `decode_set_sizes` is the only place that validates a table's shape. It
// bounds-checks every field against the mapped length and refuses an inverted or
// oversized symbol-length pair, so that `decode_pairs` — the hot path — can walk
// the stream with only the guards a corrupt file makes unavoidable. A refusal
// leaves the table unusable, which the probe reports as "no result".
//
// Mirror upstream `syzygy/tbprobe.cpp:1080` (set_sizes) and `:602`
// (decompress_pairs). Port source: zfish `platform/syzygy/decode.zig`.

#ifndef MCFISH_SYZYGY_DECODE_H
#define MCFISH_SYZYGY_DECODE_H

#include "tables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Define the upstream TBFlag bits.
enum : uint8_t {
    TB_FLAG_STM = 1,
    TB_FLAG_MAPPED = 2,
    TB_FLAG_WIN_PLIES = 4,
    TB_FLAG_LOSS_PLIES = 8,
    TB_FLAG_WIDE = 16,
    TB_FLAG_SINGLE_VALUE = 128,
};

// Parse the PairsData header at BUF[*POS], allocate base64[] and symlen[] through
// ALLOC, point the file-backed fields into BUF, and advance *POS past the btree.
// group_len/group_idx must already be filled by set_groups. Return false — leaving
// D unusable — on a short, corrupt or unallocatable table.
bool decode_set_sizes(
  PairsData *d, const uint8_t *buf, size_t buf_len, size_t *pos, SyzygyAllocFn alloc);

// Return the value stored at IDX. Report failure through *OK: a corrupt stream
// yields false and a meaningless value, never an out-of-bounds read.
int32_t decode_pairs(const PairsData *d, uint64_t idx, bool *ok);

#endif  // MCFISH_SYZYGY_DECODE_H
