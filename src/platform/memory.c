// Define _GNU_SOURCE before any libc header: madvise, MADV_HUGEPAGE and MAP_ANONYMOUS
// all sit behind glibc's __USE_MISC/__USE_GNU guards, which -D_POSIX_C_SOURCE=200809L
// alone does not open. Without it the huge-page advisory would silently compile away
// under the `#if defined(MADV_HUGEPAGE)` below -- a fallback that looks clean and is
// simply wrong. See PORT_NOTES_platform.md for the build.sh change that removes this.
#define _GNU_SOURCE

#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Assume a 2 MiB large page, as upstream does (memory.cpp:162). The value is the
// alignment AND the rounding unit: a block that is not a whole number of pages cannot
// be backed by them.
enum { LargePageSize = 2 * 1024 * 1024 };

// Reserve one 64-byte unit before the page-allocator payload: keep the payload
// 64-aligned (page memory is already 4096-aligned) and hold the block length for free().
enum { PayloadOffset = 64 };

void *std_aligned_alloc(size_t alignment, size_t size) {
    void *mem = nullptr;

    if (posix_memalign(&mem, alignment, size) != 0)
        return nullptr;

    return mem;
}

void std_aligned_free(void *ptr) { free(ptr); }

size_t large_page_size(void) { return (size_t) LargePageSize; }

void *aligned_large_pages_alloc(size_t alloc_size) {
    const size_t alignment = (size_t) LargePageSize;
    const size_t rounded_size =
      alloc_size == 0 ? 0 : ((alloc_size + alignment - 1) / alignment) * alignment;

    void *mem = std_aligned_alloc(alignment, rounded_size);
    if (mem == nullptr)
        return nullptr;

    // Zero the block. posix_memalign returns uninitialised memory; fresh OS pages happen
    // to be zero, but reused blocks (thread resize, search clear) carry stale data, and
    // the Worker has a field read during multipv search that neither its constructor nor
    // clear() initialises. Zeroing makes that field deterministically 0 -- the same value
    // a fresh-page allocation gives -- and lets Worker construction rely on zero-fill.
    memset(mem, 0, rounded_size);

    // Hint transparent huge pages. This is advisory: the kernel may ignore it, and the
    // block is already 2 MiB-aligned, so it stays valid either way.
    if (rounded_size != 0) {
#if defined(MADV_HUGEPAGE)
        (void) madvise(mem, rounded_size, MADV_HUGEPAGE);
#endif
    }

    return mem;
}

void aligned_large_pages_free(void *ptr) { std_aligned_free(ptr); }

bool has_large_pages(void) {
#if defined(MADV_HUGEPAGE)
    return true;
#else
    return false;
#endif
}

// Back the default page allocator with an anonymous mapping rather than malloc: the
// arenas here are tens of megabytes, the kernel hands them over pre-zeroed, and munmap
// returns them to the OS instead of parking them in the heap's free lists.
static void *page_alloc_default(size_t size) {
    if (size == 0)
        return nullptr;

    const size_t total = (size_t) PayloadOffset + size;
    void *raw = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return nullptr;

    // MAP_ANONYMOUS pages arrive zeroed, which is the contract's zero-fill. Record the
    // block length in the header word so free() needs no size from the caller.
    *(size_t *) raw = total;
    return (unsigned char *) raw + PayloadOffset;
}

static void page_free_default(void *ptr) {
    if (ptr == nullptr)
        return;

    unsigned char *raw = (unsigned char *) ptr - PayloadOffset;
    const size_t total = *(size_t *) (void *) raw;
    (void) munmap(raw, total);
}

static void *(*PageAllocHook)(size_t size) = page_alloc_default;
static void (*PageFreeHook)(void *ptr) = page_free_default;

void *page_alloc(size_t size) { return PageAllocHook(size); }

void page_free(void *ptr) { PageFreeHook(ptr); }

void page_alloc_set(void *(*alloc_fn)(size_t size), void (*free_fn)(void *ptr)) {
    PageAllocHook = alloc_fn != nullptr ? alloc_fn : page_alloc_default;
    PageFreeHook = free_fn != nullptr ? free_fn : page_free_default;
}
