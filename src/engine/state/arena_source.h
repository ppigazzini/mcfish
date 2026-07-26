// Inject the page allocator the engine's big arenas come from.
//
// The transposition table, the shared history bank and each SearchWorker block are
// megabyte-scale and long-lived, and how they are obtained is a HOST decision: the
// live shell backs them with an anonymous mapping so they arrive zeroed and carry a
// transparent-huge-page hint, which is a platform service the chess library must not
// reach for itself. Same shape as time_source.h and output_sink.h.
//
// THE ZERO-FILL IS PART OF THE CONTRACT, not an accident of the current backend.
// history_clear and tt_clear both rely on a fresh arena reading as zero, and
// worker_construct then overwrites every field it does not otherwise clear. An
// implementation that hands back uninitialised memory turns a forgotten field into a
// value that is right only because the allocator hid the omission.
//
// The default is malloc-backed and correct but plain: it satisfies the zeroing and
// the alignment, and gives up the huge-page hint. That keeps a headless caller --
// the test binary links no platform object -- working rather than crashing, which is
// the same bargain time_source.h makes.

#ifndef MCFISH_ARENA_SOURCE_H
#define MCFISH_ARENA_SOURCE_H

#include <stddef.h>

// Return SIZE zeroed bytes aligned to at least 64, or null. Alignment is load-bearing:
// the accumulator arena and the TT both index on the assumption their block starts on a
// cache line.
extern void *(*ArenaAlloc)(size_t size);

// Release a block from ArenaAlloc. Accept null. The pair must come from the same
// implementation -- the host registers both or neither.
extern void (*ArenaFree)(void *ptr);

#endif  // MCFISH_ARENA_SOURCE_H
