// Define _GNU_SOURCE before any libc header: madvise, MADV_HUGEPAGE and MAP_ANONYMOUS
// all sit behind glibc's __USE_MISC/__USE_GNU guards, which -D_POSIX_C_SOURCE=200809L
// alone does not open. Without it the huge-page advisory would silently compile away
// under the `#if defined(MADV_HUGEPAGE)` below -- a fallback that looks clean and is
// simply wrong.
#define _GNU_SOURCE

#include "memory.h"

#include "../engine/state/atomic.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

// Assume a 2 MiB large page, as upstream does (memory.cpp:162). The value is the
// alignment AND the rounding unit: a block that is not a whole number of pages cannot
// be backed by them.
enum { LargePageSize = 2 * 1024 * 1024 };

// Reserve one 64-byte unit before the page-allocator payload to hold the bookkeeping
// free() needs. The payload is then aligned UP to a large-page boundary, which is the
// whole point of this seam and what the previous shape quietly gave away.
//
// It used to return `mmap_base + 64`. mmap hands back a 2 MiB-aligned base for a
// mapping this size, and adding the header offset destroyed that: every arena block --
// the transposition table, the shared history bank, each worker block -- started 64
// bytes past a page boundary. Two consequences, both measured against the pinned
// oracle, whose 256 MB table IS 2 MiB-aligned where mcfish's was page-aligned only:
// transparent huge pages can never back the region (promotion needs a 2 MiB-aligned
// start, so the MADV_HUGEPAGE hint below was inert), and every cluster's cache-set
// index is skewed relative to the page it lives in.
enum { PayloadOffset = 64 };

void *std_aligned_alloc(size_t alignment, size_t size) {
    void *mem = nullptr;

    if (posix_memalign(&mem, alignment, size) != 0)
        return nullptr;

    return mem;
}

void std_aligned_free(void *ptr) { free(ptr); }

size_t large_page_size(void) { return (size_t) LargePageSize; }

// Count the mappings this file is holding: incremented on every successful map below,
// decremented on every unmap. It exists because MOVING TO mmap TOOK A LEAK GATE AWAY,
// and nothing replaced it.
//
// Neither leak checker in this tree tracks an anonymous mapping. LeakSanitizer walks the
// malloc heap, and valgrind's memcheck reports a definite leak only for allocations it
// intercepts -- so a `page_free` that does nothing is invisible to both. That is measured,
// not assumed: with `page_free_default` reduced to `(void) ptr;`, `./build.sh test` and
// `tools/valgrind.sh` both pass, and so does the whole of `./build.sh parity` -- twenty
// gates green over a build that never releases an arena.
//
// The counter is the replacement, and `test_page_allocator` is what reads it. Relaxed is
// the right order for a statistic: it is atomic so a concurrent resize cannot tear it,
// and the test that reads it is single-threaded. Static storage zero-initialises it,
// which is the value it must start at.
static AtomicU64 LiveMappings;

uint64_t memory_live_mappings(void) { return atomic_u64_load(&LiveMappings); }

// Back every large block with an anonymous mapping rather than with malloc, and serve
// both surfaces below from here: the blocks are tens of megabytes, the kernel hands
// them over pre-zeroed, and munmap returns them to the OS instead of parking them in
// the heap's free lists where a later thread on another NUMA node can be handed one.
static void *map_large_aligned(size_t size) {
    if (size == 0)
        return nullptr;

    // Over-allocate by one large page so the PAYLOAD -- not the mapping -- can start on
    // a large-page boundary with the header still ahead of it.
    const size_t align = (size_t) LargePageSize;
    const size_t total = size + align;
    void *raw = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return nullptr;

    // First large-page boundary at least PayloadOffset past the mapping, so the two
    // header words always fit between `raw` and the payload.
    uintptr_t addr = (uintptr_t) raw + (uintptr_t) PayloadOffset;
    addr = (addr + align - 1) & ~(uintptr_t) (align - 1);
    unsigned char *const payload = (unsigned char *) addr;

    // Hint transparent huge pages once the mapping is large enough to hold at least one.
    // The big arenas that ride this helper -- the 16 MiB transposition table, the per-node
    // shared-history banks -- are exactly L3-sized, so at 4 KiB pages the search walks
    // thousands of TLB entries across them; a 2 MiB page is one entry for the same span.
    // The hint is advisory: the kernel may ignore it (WSL2 here backs none of it), and it
    // can only change WHERE the block is paged, never a byte of its content -- MAP_ANONYMOUS
    // still hands the region over zeroed either way. Guard on MADV_HUGEPAGE so the call
    // compiles away where the constant is undefined.
#if defined(MADV_HUGEPAGE)
    // Advise on the ALIGNED payload, which is the region the kernel can actually back
    // with huge pages now that it starts on a boundary.
    if (size >= (size_t) LargePageSize)
        (void) madvise(payload, size, MADV_HUGEPAGE);
#endif

    // MAP_ANONYMOUS pages arrive zeroed, which is the contract's zero-fill. Record the
    // mapping base and length in the two words ahead of the payload, so free() needs
    // neither a size from the caller nor a fixed offset back to the mapping.
    ((size_t *) payload)[-1] = total;
    ((void **) payload)[-2] = raw;
    (void) atomic_u64_fetch_add(&LiveMappings, 1);
    return payload;
}

static void unmap_large_aligned(void *ptr) {
    if (ptr == nullptr)
        return;

    // The payload is aligned, not at a fixed offset from the mapping, so both the
    // base and the length come out of the two header words ahead of it.
    unsigned char *const payload = (unsigned char *) ptr;
    const size_t total = ((size_t *) (void *) payload)[-1];
    void *const raw = ((void **) (void *) payload)[-2];
    (void) munmap(raw, total);
    (void) atomic_u64_fetch_sub(&LiveMappings, 1);
}

void *aligned_large_pages_alloc(size_t alloc_size) {
    const size_t alignment = (size_t) LargePageSize;
    const size_t rounded_size =
      alloc_size == 0 ? 0 : ((alloc_size + alignment - 1) / alignment) * alignment;

    if (rounded_size == 0)
        return nullptr;

    // MAP the block rather than taking it from a malloc arena, as upstream's
    // aligned_large_pages_alloc_with_hint does since 7ab49b9b. A glibc arena outlives
    // the thread that created it and can be handed to a thread bound to a DIFFERENT
    // NUMA node later, so a block served from one is remote memory the engine cannot
    // see or place -- which is what made a second `setoption name Threads` measurably
    // slower than the first upstream. A mapping is placed by first touch, every time.
    //
    // Same helper the page_alloc seam below uses: the two words ahead of the payload
    // carry the mapping's base and length, so the free needs no size from the caller
    // and no registry keyed on the pointer.
    //
    // The block is still UNINITIALISED by contract, as upstream's is (memory.cpp:129
    // onward -- neither the mmap path nor the aligned_alloc fallback promises zeroes).
    // Anonymous pages happen to arrive zeroed; a caller that reads a field it never
    // wrote is a bug, and an allocator that guaranteed zeroes would hide it behind a
    // plausible value.
    return map_large_aligned(rounded_size);
}

void aligned_large_pages_free(void *ptr) { unmap_large_aligned(ptr); }

bool has_large_pages(void) {
#if defined(MADV_HUGEPAGE)
    return true;
#else
    return false;
#endif
}

static void *page_alloc_default(size_t size) { return map_large_aligned(size); }

static void page_free_default(void *ptr) { unmap_large_aligned(ptr); }

static void *(*PageAllocHook)(size_t size) = page_alloc_default;
static void (*PageFreeHook)(void *ptr) = page_free_default;

void *page_alloc(size_t size) { return PageAllocHook(size); }

void page_free(void *ptr) { PageFreeHook(ptr); }

void page_alloc_set(void *(*alloc_fn)(size_t size), void (*free_fn)(void *ptr)) {
    PageAllocHook = alloc_fn != nullptr ? alloc_fn : page_alloc_default;
    PageFreeHook = free_fn != nullptr ? free_fn : page_free_default;
}
