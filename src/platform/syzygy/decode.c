#include "decode.h"

#include <string.h>

// Report whether NEED bytes are readable at BUF[P].
static inline bool fits(size_t p, size_t need, size_t buf_len) {
    return need <= buf_len && p <= buf_len - need;
}

// Port upstream `set_sizes` (syzygy/tbprobe.cpp:1080).
bool decode_set_sizes(
  PairsData *d, const uint8_t *buf, size_t buf_len, size_t *pos, SyzygyAllocFn alloc) {
    size_t p = *pos;

    if (!fits(p, 1, buf_len)) {
        return false;
    }
    d->flags = buf[p];
    p += 1;

    if (d->flags & TB_FLAG_SINGLE_VALUE) {
        d->blocks_num = 0;
        d->block_length_size = 0;
        d->span = 0;
        d->sparse_index_size = 0;
        if (!fits(p, 1, buf_len)) {
            return false;
        }
        d->min_sym_len = buf[p];  // the single stored value
        p += 1;
        *pos = p;
        return true;
    }

    // Take tb_size from group_idx at the group_len[] terminator index.
    size_t term = 0;
    while (term < TB_PIECES && d->group_len[term] != 0) {
        ++term;
    }
    const uint64_t tb_size = d->group_idx[term];

    if (!fits(p, 3 + 4 + 2, buf_len)) {
        return false;
    }
    if (buf[p] >= 64 || buf[p + 1] >= 64) {
        return false;  // a block or span log that does not fit a size_t shift
    }
    d->sizeof_block = (size_t) 1 << buf[p];
    p += 1;
    d->span = (size_t) 1 << buf[p];
    p += 1;
    d->sparse_index_size = (size_t) ((tb_size + d->span - 1) / d->span);  // round up
    const uint8_t padding = buf[p];
    p += 1;
    d->blocks_num = rd_u32le(buf + p);
    p += 4;
    // Both terms are file bytes and the sum is 32-bit, so it can wrap — to a bound
    // SMALLER than blocks_num, which the block walk would then read as a table with
    // fewer block lengths than blocks. Saturate: a table claiming more blocks than
    // the count can hold is corrupt either way, and the later width check refuses
    // it on its own terms.
    d->block_length_size =
      d->blocks_num > UINT32_MAX - padding ? UINT32_MAX : d->blocks_num + padding;
    d->max_sym_len = buf[p];
    p += 1;
    d->min_sym_len = buf[p];
    p += 1;
    d->lowest_sym = buf + p;  // Sym[] inside the file

    // Both symbol lengths are raw file bytes. Refuse an inverted or oversized pair
    // before the arithmetic below: max < min underflows the length subtraction, a
    // length at 64 or beyond drives the right-pad shift out of range, and a zero
    // minimum makes that shift exactly 64 — undefined for a 64-bit shift in C.
    if (d->min_sym_len == 0 || d->min_sym_len > d->max_sym_len || d->max_sym_len >= 64) {
        return false;
    }

    const size_t base64_size = (size_t) (d->max_sym_len - d->min_sym_len) + 1;
    // The loop below reads `lowest_sym[i]` and `lowest_sym[i + 1]` for i down from
    // base64_size - 2, so the highest entry touched is base64_size - 1 and the last
    // byte touched is at offset base64_size * 2 - 1. Demanding one entry more would
    // reject a table whose lowest_sym[] ends exactly at the mapping's end.
    if (!fits(p, base64_size * 2, buf_len)) {
        return false;
    }
    d->base64 = alloc(base64_size * sizeof(uint64_t));
    if (d->base64 == nullptr) {
        return false;
    }
    d->base64_size = base64_size;

    // Build the canonical Huffman bases: base64[i] >= base64[i+1], last one 0.
    // The intermediate sum is deliberately unsigned 64-bit arithmetic, as upstream:
    // the difference of the two symbol bases may wrap, and the wrap is part of the
    // computation, not an error.
    d->base64[base64_size - 1] = 0;
    for (size_t i = base64_size - 1; i > 0;) {
        --i;
        d->base64[i] = (d->base64[i + 1] + (uint64_t) rd_sym(d->lowest_sym + i * 2)
                        - (uint64_t) rd_sym(d->lowest_sym + (i + 1) * 2))
                     / 2;
    }
    // Right-pad to 64 bits. base64_size == max_sym_len - min_sym_len + 1 with
    // max_sym_len < 64 and min_sym_len >= 1, so the shift stays in 1..63.
    for (size_t k = 0; k < base64_size; ++k) {
        d->base64[k] <<= (unsigned) (64 - k - d->min_sym_len);
    }

    p += base64_size * 2;  // sizeof(Sym)
    if (!fits(p, 2, buf_len)) {
        return false;
    }
    const size_t symlen_size = rd_u16le(buf + p);
    p += 2;
    if (!fits(p, symlen_size * sizeof(LR), buf_len)) {
        return false;
    }
    // Read the btree through an LR* into the mapped file. LR is three bytes with
    // alignment 1, so the aim is valid at any offset.
    d->btree = (const LR *) (const void *) (buf + p);
    d->btree_size = symlen_size;

    d->symlen = alloc(symlen_size + 1);  // +1: keep a zero-length table allocatable
    if (d->symlen == nullptr) {
        return false;
    }
    d->symlen_size = symlen_size;
    memset(d->symlen, 0, symlen_size + 1);

    bool *visited = alloc(symlen_size + 1);
    if (visited == nullptr) {
        return false;
    }
    memset(visited, 0, symlen_size + 1);
    for (size_t sym = 0; sym < symlen_size; ++sym) {
        if (!visited[sym]) {
            d->symlen[sym] = set_sym_len(d, (Sym) sym, visited);
        }
    }

    p += symlen_size * sizeof(LR) + (symlen_size & 1);
    *pos = p;
    return true;
}

// Port upstream `decompress_pairs` (syzygy/tbprobe.cpp:602). The guards on the
// block walk, the base64 scan and the symbol index cannot fire on a well-formed
// table; they exist so a corrupt one reports failure instead of reading past the
// mapping.
int32_t decode_pairs(const PairsData *d, uint64_t idx, bool *ok) {
    *ok = true;
    if (d->flags & TB_FLAG_SINGLE_VALUE) {
        return d->min_sym_len;
    }

    if (d->span == 0 || d->sparse_index == nullptr || d->block_length == nullptr
        || d->data == nullptr || d->base64_size == 0) {
        *ok = false;
        return 0;
    }

    // Locate the block through the sparse index, then walk block_length[] to it.
    const uint64_t k = idx / d->span;
    if (k >= d->sparse_index_size) {
        *ok = false;
        return 0;
    }
    const uint8_t *const entry = d->sparse_index + (size_t) k * sizeof(SparseEntry);
    uint32_t block = rd_u32le(entry);                // SparseEntry.block
    int32_t offset = (int32_t) rd_u16le(entry + 4);  // SparseEntry.offset

    // Subtract in unsigned 64-bit and narrow ONCE, as upstream does
    // (syzygy/tbprobe.cpp:634 `int diff = int(idx % d->span - d->span / 2)`).
    // Narrowing each side first and subtracting in int32_t is not the same
    // computation: `span` is `1 << b` for a file byte b up to 63, so either side
    // can exceed INT32_MAX on a crafted table and the signed subtraction is then
    // undefined. Unsigned wrap is defined, and the single narrowing that follows
    // reproduces upstream's value bit for bit.
    const int32_t diff = (int32_t) ((idx % d->span) - (d->span / 2));
    offset += diff;

    // `block` is a raw 32-bit field of the compressed payload, so nothing the
    // header stated bounds it. Refuse it here, before either walk: the backward
    // one only ever decrements, so one test covers every index it reads, and the
    // forward one keeps its own because it increments. A well-formed table always
    // satisfies this — block_length_size is blocks_num plus the padding.
    if (block >= d->block_length_size) {
        *ok = false;
        return 0;
    }

    const uint8_t *const bl = d->block_length;
    while (offset < 0) {
        if (block == 0) {
            *ok = false;
            return 0;
        }
        block -= 1;
        offset += (int32_t) rd_u16le(bl + (size_t) block * 2) + 1;
    }
    while (true) {
        if (block >= d->block_length_size) {
            *ok = false;
            return 0;
        }
        const int32_t len = (int32_t) rd_u16le(bl + (size_t) block * 2);
        if (offset <= len) {
            break;
        }
        offset -= len + 1;
        block += 1;
    }
    if (block >= d->blocks_num) {
        *ok = false;
        return 0;
    }

    // Read the block's canonical-Huffman bitstream through 64-bit big-endian windows.
    const uint8_t *const end = d->data + d->data_size;
    const uint8_t *ptr = d->data + (uint64_t) block * d->sizeof_block;
    if (ptr > end || (size_t) (end - ptr) < 8) {
        *ok = false;
        return 0;
    }
    uint64_t buf64 = rd_u64be(ptr);
    ptr += 8;
    int32_t buf64_size = 64;
    Sym sym = 0;

    while (true) {
        int32_t len = 0;  // symbol length - min_sym_len
        while (buf64 < d->base64[len]) {
            ++len;
            if ((size_t) len >= d->base64_size) {
                *ok = false;
                return 0;
            }
        }
        const int32_t shift = 64 - len - (int32_t) d->min_sym_len;
        if (shift <= 0 || shift >= 64) {
            *ok = false;
            return 0;
        }
        sym = (Sym) ((buf64 - d->base64[len]) >> (unsigned) shift);
        sym = (Sym) (sym + rd_sym(d->lowest_sym + (size_t) len * 2));

        if ((size_t) sym >= d->symlen_size) {
            *ok = false;
            return 0;
        }
        if (offset < (int32_t) d->symlen[sym] + 1) {
            break;
        }

        offset -= (int32_t) d->symlen[sym] + 1;
        len += (int32_t) d->min_sym_len;  // the real length
        buf64 <<= (unsigned) len;
        buf64_size -= len;
        if (buf64_size <= 32) {
            buf64_size += 32;
            // Refuse a window the symbol lengths have driven out of range. A valid
            // table never consumes more bits than the window holds, so this stays
            // above zero; a corrupt one can decode symbols long enough to walk it
            // negative, and the refill shift below would then be 64 or more —
            // undefined for a 64-bit type in C.
            if (buf64_size <= 0) {
                *ok = false;
                return 0;
            }
            if ((size_t) (end - ptr) < 4) {
                *ok = false;
                return 0;
            }
            buf64 |= (uint64_t) rd_u32be(ptr) << (unsigned) (64 - buf64_size);
            ptr += 4;
        }
    }

    // Expand the symbol down to the leaf that holds the value.
    while (d->symlen[sym] != 0) {
        if ((size_t) sym >= d->btree_size) {
            *ok = false;
            return 0;
        }
        const uint8_t here = d->symlen[sym];
        const Sym left = lr_left(d->btree[sym]);
        if ((size_t) left >= d->symlen_size) {
            *ok = false;
            return 0;
        }
        Sym next;
        if (offset < (int32_t) d->symlen[left] + 1) {
            next = left;
        } else {
            offset -= (int32_t) d->symlen[left] + 1;
            next = lr_right(d->btree[sym]);
            if ((size_t) next >= d->symlen_size) {
                *ok = false;
                return 0;
            }
        }
        // Descend only while the symbol shrinks. `set_sym_len` gives a well-formed
        // btree symlen[s] == symlen[left] + symlen[right] + 1, so every step of a
        // valid table already strictly decreases this and the test never fires. A
        // corrupt btree can name a child that does not — including the node itself
        // — and the walk would then never terminate: the tree is acyclic only
        // because a valid file says so, and nothing here re-derives it.
        if (d->symlen[next] >= here) {
            *ok = false;
            return 0;
        }
        sym = next;
    }
    if ((size_t) sym >= d->btree_size) {
        *ok = false;
        return 0;
    }
    return lr_left(d->btree[sym]);
}
