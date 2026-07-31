// Fuzz the Syzygy table parse and the RE-PAIR decoder over arbitrary bytes.
//
// A `.rtbw`/`.rtbz` is the only attacker-supplyable BINARY input the engine
// parses besides the net -- SyzygyPath names a file the engine did not write --
// and every offset the parse advances is read out of that file. The bounds in
// decode.c and registry.c are therefore a security boundary, not a tidiness
// claim, and a boundary with no fuzzer behind it is a wish: standing this driver
// up found three bugs the hand-written bounds had missed, two of them in the
// decoder that runs inside the search (an out-of-bounds read through the
// backward block walk, a non-terminating btree descent, and a shift of 64).
//
// Link only decode.c, tables.c and encode.c. They depend on nothing but libc, so
// this needs neither ENGINE_SOURCES nor a shell -- unlike tools/fuzz_search.c,
// which links the engine because the search is what it drives. The small link is
// deliberate beyond convenience: it keeps the iteration rate high enough that the
// interesting states are header shapes rather than startup.
//
// The driver carves the three file-backed regions EXACTLY as registry.c `set`
// does, rather than clamping them to whatever is left. A harness that hands the
// decoder a region the registry would have rejected reports failures that cannot
// happen, and a fuzzer whose crashes are not reachable is worse than none.
//
// The file lives in an exact-size heap allocation, so ASan's redzones bound it
// to the byte. Production maps the file with mmap, where a read just past the
// end lands in the page padding and is silently zero -- this is STRICTER than
// the shipped engine on purpose, because that padding is what turns an
// out-of-bounds read into a bug nobody sees.
//
// Golden: none. This is test infrastructure, not a port.

#include "../src/platform/syzygy/decode.h"
#include "../src/platform/syzygy/tables.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The libFuzzer runtime calls this by name; no mcfish header declares it because
// no mcfish translation unit calls it. Declare it here, once, so
// -Wmissing-prototypes has the header-in-the-same-file every other symbol in this
// tree gets (docs/08-idiomatic-c.md's warning table).
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Track this input's allocations so each one gets its own ASan redzone. The
// shipped registry hands `set` an arena and frees the generation wholesale, but a
// bump arena would put base64[] and symlen[] shoulder to shoulder, and an
// out-of-bounds WRITE into symlen[] -- the exact shape a corrupt btree produces
// -- would land in a neighbour instead of a redzone and go unreported.
enum { FUZZ_MAX_BLOCKS = 16 };  // decode_set_sizes takes three per call

static void *Blocks[FUZZ_MAX_BLOCKS];
static size_t BlockCount = 0;

static void *fuzz_alloc(size_t bytes) {
    if (BlockCount == FUZZ_MAX_BLOCKS) {
        return nullptr;
    }
    void *p = malloc(bytes == 0 ? 1 : bytes);
    if (p == nullptr) {
        return nullptr;
    }
    Blocks[BlockCount++] = p;
    return p;
}

static void fuzz_free_all(void) {
    for (size_t i = 0; i < BlockCount; ++i) {
        free(Blocks[i]);
    }
    BlockCount = 0;
}

// Mirror registry.c's own bound. Kept as a copy rather than exported: the point
// is to fuzz the parse, and a shared helper would let a bug in the bound hide
// itself from the driver that is supposed to find it.
static bool fits(size_t pos, size_t bytes, size_t len) {
    return bytes <= len && pos <= len - bytes;
}

// Give the PairsData the group state decode_set_sizes reads: the group_len
// terminator selects the group_idx entry it takes tb_size from. Keep the values
// small and self-consistent -- the target is the FILE bytes, and an absurd
// tb_size only starves the header parse of the branches worth exploring.
static void seed_groups(PairsData *d, uint8_t seed) {
    d->group_len[0] = 1;
    d->group_idx[0] = 1;
    d->group_idx[1] = (uint64_t) seed + 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) {
        return 0;
    }
    const uint8_t seed = data[0];
    const uint8_t idx_seed = data[1];
    data += 2;
    size -= 2;

    uint8_t *buf = malloc(size);
    if (buf == nullptr) {
        return 0;
    }
    memcpy(buf, data, size);

    PairsData d;
    memset(&d, 0, sizeof d);
    seed_groups(&d, seed);

    size_t pos = 0;
    if (!decode_set_sizes(&d, buf, size, &pos, fuzz_alloc)) {
        goto done;
    }

    // A parse that succeeded must have stayed inside the file. The cursor may
    // sit exactly one past the end -- decode_set_sizes word-aligns past the
    // btree and does not require that pad byte to exist, which upstream does not
    // either -- and every consumer tests the cursor before using it.
    if (pos > size + 1) {
        abort();  // cursor past the end of the file
    }
    if (d.lowest_sym != nullptr && (d.lowest_sym < buf || d.lowest_sym > buf + size)) {
        abort();  // lowest_sym outside the file
    }
    if (d.btree_size != 0) {
        const uint8_t *const bt = (const uint8_t *) (const void *) d.btree;
        if (bt < buf || !fits((size_t) (bt - buf), d.btree_size * sizeof(LR), size)) {
            abort();  // btree outside the file
        }
    }

    // Carve the regions as registry.c `set` does, and give up where it would.
    size_t span;
    if (__builtin_mul_overflow(d.sparse_index_size, sizeof(SparseEntry), &span)
        || !fits(pos, span, size)) {
        goto done;
    }
    d.sparse_index = buf + pos;
    pos += span;

    span = (size_t) d.block_length_size * 2;
    if (!fits(pos, span, size)) {
        goto done;
    }
    d.block_length = buf + pos;
    pos += span;

    pos = (pos + 0x3F) & ~(size_t) 0x3F;  // 64-byte alignment
    if (__builtin_mul_overflow((size_t) d.blocks_num, d.sizeof_block, &span)
        || !fits(pos, span, size)) {
        goto done;
    }
    d.data = buf + pos;
    d.data_size = span;

    // Decode at indices the input picks. The VALUE is unconstrained -- a garbage
    // table decodes to garbage, and decode_pairs reports that through *ok -- so
    // require only that the walk terminates without leaving the regions.
    for (uint64_t i = 0; i < 4; ++i) {
        bool ok = false;
        (void) decode_pairs(&d, (uint64_t) idx_seed + i * 7919u, &ok);
    }

done:
    fuzz_free_all();
    free(buf);
    return 0;
}
