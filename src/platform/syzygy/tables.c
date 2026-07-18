#include "tables.h"

#include "encode.h"

// Port upstream `set_groups` (syzygy/tbprobe.cpp:1006). The piece sequence in
// d->pieces is split into groups of interchangeable men; group_len[] records the
// sizes and group_idx[] the multiplier each group contributes to the index.
void set_groups(PairsData *d, EntryInfo e, const int32_t order[2], size_t f) {
    size_t n = 0;
    int32_t first_len = e.has_pawns ? 0 : (e.has_unique_pieces ? 3 : 2);
    d->group_len[0] = 1;

    for (size_t i = 1; i < (size_t) e.piece_count && i < TB_PIECES; ++i) {
        --first_len;
        if (first_len > 0 || d->pieces[i] == d->pieces[i - 1]) {
            ++d->group_len[n];
        } else {
            ++n;
            d->group_len[n] = 1;
        }
    }
    ++n;
    d->group_len[n] = 0;  // zero-terminated

    const bool pp = e.has_pawns && e.pawn_count[1] != 0;  // pawns on both sides
    size_t next = pp ? 2 : 1;
    int32_t free_squares = 64 - d->group_len[0] - (pp ? d->group_len[1] : 0);
    uint64_t idx = 1;

    for (int32_t k = 0; next < n || k == order[0] || k == order[1]; ++k) {
        if (k == order[0]) {  // leading pawns or pieces
            d->group_idx[0] = idx;
            uint64_t mult;
            if (e.has_pawns) {
                mult = (uint64_t) lead_pawns_size_at(d->group_len[0], f);
            } else if (e.has_unique_pieces) {
                mult = 31332;
            } else {
                mult = 462;
            }
            idx *= mult;
        } else if (k == order[1]) {  // remaining pawns
            d->group_idx[1] = idx;
            idx *= (uint64_t) binomial_at(d->group_len[1], 48 - d->group_len[0]);
        } else {  // remaining pieces
            d->group_idx[next] = idx;
            idx *= (uint64_t) binomial_at(d->group_len[next], free_squares);
            free_squares -= d->group_len[next];
            ++next;
        }
    }
    d->group_idx[n] = idx;
}

// Port upstream `set_symlen` (syzygy/tbprobe.cpp:1061). Guard the symbol index:
// a corrupt btree must fail soft rather than read past the mapped file.
uint8_t set_sym_len(PairsData *d, Sym s, bool *visited) {
    if ((size_t) s >= d->btree_size) {
        return 0;
    }
    visited[s] = true;  // safe here: the tree is acyclic

    const Sym sr = lr_right(d->btree[s]);
    if (sr == 0xFFF) {
        return 0;  // leaf
    }

    const Sym sl = lr_left(d->btree[s]);
    if ((size_t) sl >= d->symlen_size || (size_t) sr >= d->symlen_size) {
        return 0;
    }
    if (!visited[sl]) {
        d->symlen[sl] = set_sym_len(d, sl, visited);
    }
    if (!visited[sr]) {
        d->symlen[sr] = set_sym_len(d, sr, visited);
    }
    return (uint8_t) (d->symlen[sl] + d->symlen[sr] + 1);
}
