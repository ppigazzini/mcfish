// Own every large, long-lived allocation the engine makes, and the page-allocator seam.
//
// Hand back blocks that are 2 MiB-aligned and rounded up to a whole number of large
// pages. The alignment is load-bearing: the NNUE accumulator reads it as a precondition.
// The CONTENTS are not initialised, exactly as upstream leaves them -- a caller that
// reads a field it never wrote is a bug in the caller, and an allocator that zeroes hides
// it behind a plausible value. Degrade to a plain aligned allocation on a host without
// huge pages; never fail an allocation malloc could serve.

// The page_alloc seam below is the exception, and states its zero-fill explicitly: it is
// backed by an anonymous mapping, which the kernel is required to hand over zeroed.
//
// Upstream: memory.cpp:71 (std_aligned_alloc), memory.cpp:151
// (aligned_large_pages_alloc_with_hint), memory.cpp:182 (has_large_pages).
// Port source: zfish src/platform/memory.zig, src/engine/state/page_alloc.zig.

#ifndef CCFISH_MEMORY_H
#define CCFISH_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

// Return SIZE bytes aligned to ALIGNMENT, or nullptr. ALIGNMENT must be a power of two
// and a multiple of sizeof(void *). The block is uninitialised.
void *std_aligned_alloc(size_t alignment, size_t size);

// Release a block from std_aligned_alloc. A nullptr is a no-op.
void std_aligned_free(void *ptr);

// Return an UNINITIALISED, 2 MiB-aligned block of at least ALLOC_SIZE bytes, or nullptr.
// The size is rounded up to a whole number of 2 MiB pages, so the block may be larger
// than requested; callers must not assume the rounding away.
void *aligned_large_pages_alloc(size_t alloc_size);

// Release a block from aligned_large_pages_alloc. A nullptr is a no-op.
void aligned_large_pages_free(void *ptr);

// Report whether the host offers a large-page advisory at all. False means the
// allocation above is still correct, only without the TLB win.
bool has_large_pages(void);

// Return the rounding the large-page allocator applies: the alignment, in bytes.
size_t large_page_size(void);

// Allocate a zeroed block of SIZE bytes whose payload is at least 64-byte aligned, or
// nullptr. Route the engine's big arenas (transposition table, shared histories, NNUE
// weight storage) through here so the engine zone never names an OS allocator.
void *page_alloc(size_t size);

// Release a block from page_alloc. Pointer only -- the size is the allocator's business.
void page_free(void *ptr);

// Install a replacement page allocator. Register the pair together or not at all: the
// default free knows only the default alloc's block header. Pass nullptr for either to
// restore the default. Call this before any page_alloc, never between an alloc and its
// free.
void page_alloc_set(void *(*alloc_fn)(size_t size), void (*free_fn)(void *ptr));

#endif  // CCFISH_MEMORY_H
