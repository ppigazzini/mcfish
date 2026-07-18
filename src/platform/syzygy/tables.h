// Own the Syzygy on-disk data model: the RE-PAIR btree entry, the sparse-index
// entry, the per-(side, file) PairsData record, and the two pure layout helpers
// `set_groups` and `set_sym_len`.
//
// Every pointer field of a PairsData aims into a mapped table file and is valid
// only while that mapping lives — registry.c fills them and owns the lifetime.
// Read every multi-byte field of a table file through the rd_* helpers below:
// the file's headers and indices are little-endian and its compressed blocks are
// big-endian, on every host, and the mapped bytes are unaligned. Casting a mapped
// pointer to a wider type is therefore never correct here.
//
// Mirror upstream `syzygy/tbprobe.cpp:192` (SparseEntry), `:201` (LR), `:351`
// (PairsData), `:1006` (set_groups), `:1061` (set_symlen).
// Port source: zfish `platform/syzygy/probe.zig`.

#ifndef CCFISH_SYZYGY_TABLES_H
#define CCFISH_SYZYGY_TABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { TB_PIECES = 7 };  // upstream TBPIECES: the largest table supported

typedef uint16_t Sym;  // RE-PAIR / canonical-Huffman symbol

// Pack two 12-bit symbols (left child, right child) into 3 bytes. When the symbol
// has length 1 the left field is the stored value; right == 0xFFF marks a leaf.
typedef struct {
    uint8_t lr[3];
} LR;

static_assert(sizeof(LR) == 3, "LR must be exactly 3 bytes: it overlays the file");

static inline Sym lr_left(LR e) { return (Sym) (((Sym) (e.lr[1] & 0xF) << 8) | e.lr[0]); }
static inline Sym lr_right(LR e) { return (Sym) (((Sym) e.lr[2] << 4) | (e.lr[1] >> 4)); }

// Hold a partial index into block_length[] — upstream's `char block[4]; offset[2]`,
// read little-endian at access time so the record is exactly 6 bytes, unpadded.
typedef struct {
    uint8_t block[4];
    uint8_t offset[2];
} SparseEntry;

static_assert(sizeof(SparseEntry) == 6, "SparseEntry must be exactly 6 bytes");

// Hold the indexing and decompression state for one (side, file) of a table.
typedef struct PairsData {
    uint8_t flags;
    uint8_t max_sym_len;
    uint8_t min_sym_len;
    uint32_t blocks_num;
    size_t sizeof_block;
    size_t span;
    const uint8_t *lowest_sym;  // Sym[] inside the file (unaligned, little-endian)
    const LR *btree;            // LR[] inside the file
    size_t btree_size;
    const uint8_t *block_length;  // uint16_t[] inside the file
    uint32_t block_length_size;
    const uint8_t *sparse_index;  // SparseEntry[] inside the file
    size_t sparse_index_size;
    const uint8_t *data;  // the compressed blocks
    size_t data_size;     // bytes of `data` inside the file, for bounds checks
    uint64_t *base64;     // owned by the registry arena
    size_t base64_size;
    uint8_t *symlen;  // owned by the registry arena
    size_t symlen_size;
    uint8_t pieces[TB_PIECES];
    uint64_t group_idx[TB_PIECES + 1];
    int32_t group_len[TB_PIECES + 1];
    uint16_t map_idx[4];
} PairsData;

// Hold the per-table metadata derived from the material configuration at init.
typedef struct {
    bool has_pawns;
    bool has_unique_pieces;
    int32_t piece_count;
    uint8_t pawn_count[2];  // [leading color, other color]
} EntryInfo;

// Allocate zeroed bytes that live until the registry is reset. Return nullptr on
// failure; every caller must treat that as "table unavailable".
typedef void *(*SyzygyAllocFn)(size_t bytes);

// Read the unaligned, explicitly-ordered file scalars. Assemble byte by byte: a
// widening load through a mapped pointer would be both unaligned and
// host-endian-dependent.
static inline uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t) ((uint16_t) p[0] | (uint16_t) ((uint16_t) p[1] << 8));
}

static inline uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
         | ((uint32_t) p[3] << 24);
}

static inline uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8)
         | (uint32_t) p[3];
}

static inline uint64_t rd_u64be(const uint8_t *p) {
    return ((uint64_t) rd_u32be(p) << 32) | (uint64_t) rd_u32be(p + 4);
}

static inline Sym rd_sym(const uint8_t *p) { return (Sym) rd_u16le(p); }

// Split the piece sequence in D->pieces into encoding groups: fill group_len[]
// (zero-terminated) and group_idx[] (each group's multiplicative start index).
// ORDER and F come from the file header. Require the geometry tables to be built.
void set_groups(PairsData *d, EntryInfo e, const int32_t order[2], size_t f);

// Expand btree symbol S down to its leaves, returning the number of values it
// represents minus one, and filling d->symlen. VISITED guards re-entry; the tree
// is acyclic, so the recursion terminates.
uint8_t set_sym_len(PairsData *d, Sym s, bool *visited);

#endif  // CCFISH_SYZYGY_TABLES_H
