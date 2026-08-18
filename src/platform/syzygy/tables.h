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

#ifndef MCFISH_SYZYGY_TABLES_H
#define MCFISH_SYZYGY_TABLES_H

#include <stddef.h>
#include <stdint.h>

enum { TB_PIECES = 7 };  // upstream TBPIECES: the largest table supported

// How many top bits of the bitstream word PairsData::len_tab may index, and the
// entry meaning "this bucket holds no single length; take the scan".
//
// Uncapped the table wants 2^63 entries. Under the cap the FIRST length past it
// divides a bucket, so that length and everything below it stay SYZYGY_NO_FAST_LEN.
// The marker cannot collide with a length: a length here is base64[]'s own index,
// and decode_set_sizes refuses a table with max_sym_len at 64 or above.
enum { SYZYGY_LEN_TAB_MAX_BITS = 12, SYZYGY_NO_FAST_LEN = 0xFF };

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

    // Per-LENGTH decode values, filled by decode_set_sizes and indexed by the length
    // the base64[] scan returns. A table has at most 63 distinct symbol lengths
    // (base64_size == max_sym_len - min_sym_len + 1, with min >= 1 and max < 64), so
    // 64 entries cover every length the scan can produce.
    //
    // Held INLINE rather than behind a pointer: the decode loop already runs out of
    // registers, and reaching each table off `d` folds the index into the load's own
    // addressing instead of spending a register on a base pointer.
    uint8_t len_shift[64];    // 64 - len - min_sym_len, always in [1, 63]
    uint16_t len_offset[64];  // lowest_sym[len] - (base64[len] >> shift), mod 2^16
    uint8_t len_real[64];     // len + min_sym_len: the bits the symbol consumes

    // Answer the base64[] scan with ONE load, indexed by the top bits of the
    // bitstream word. A code no longer than K bits owns a WHOLE NUMBER of buckets of
    // those bits, because base64[] is right-padded to 64: a length's span runs from
    // its own base to one below its predecessor's, and both ends land on a bucket
    // boundary while the length fits in the index. So the fill is exact rather than
    // approximate, and a bucket the fill could not decide says SYZYGY_NO_FAST_LEN and
    // reaches the scan.
    //
    // Sized to the TABLE's own max_sym_len, not to the cap: the buckets the stream
    // reaches are the whole table either way, so sizing to the cap would only touch
    // more cache lines. Behind a pointer, unlike the three above -- at the cap it is
    // 4 KiB, which is not something to hold inline in every PairsData.
    uint8_t *len_tab;       // owned by the registry arena
    uint8_t len_tab_shift;  // 64 minus how many top bits len_tab indexes
    uint8_t escape_len;     // the first length len_tab cannot answer for
    uint8_t *symlen;        // owned by the registry arena
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
void set_groups(PairsData *d, EntryInfo e, const int32_t order[static 2], size_t f);

// Expand btree symbol S down to its leaves, returning the number of values it
// represents minus one, and filling d->symlen. VISITED guards re-entry; the tree
// is acyclic, so the recursion terminates.
uint8_t set_sym_len(PairsData *d, Sym s, bool *visited);

#endif  // MCFISH_SYZYGY_TABLES_H
