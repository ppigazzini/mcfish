// Own the UCI `go` search-limits record: the searchmoves filter, the seven clocks, the
// five search-mode counters, the node cap and the ponder flag.
//
// The invariant is that this stays a POD leaf. It names the value domain and nothing
// else -- no Position, no ThreadPool, no search state -- because the `go` parser fills it
// before any worker exists and the worker copies it field by field, never as a byte range.
// A `searchmoves` entry is a fixed 8-byte text record rather than a heap string so the
// record has no owned memory and a copy is a copy.
//
// `start_time` is set once by the `go` handler from the wall clock. Nothing inside the
// search may read it to decide which nodes to visit -- only the time manager reads it --
// or the single-threaded node count stops being reproducible.
//
// Upstream: search.h:160 (LimitsType), uci.cpp (parse of `go`). Port source: zfish
// src/engine/state/limits_type.zig.

#ifndef MCFISH_LIMITS_TYPE_H
#define MCFISH_LIMITS_TYPE_H

#include "../board/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Hold one `go searchmoves` element: a length plus up to 7 characters, which covers
// every UCI move text ("e2e4", "e7e8q"). Sized so the record is 8 bytes with no padding.
enum { SEARCHMOVE_TEXT_MAX = 7 };

typedef struct {
    uint8_t len;
    char text[SEARCHMOVE_TEXT_MAX];
} SearchMoveText;

static_assert(sizeof(SearchMoveText) == 8, "SearchMoveText must stay an 8-byte record");

// Carry a point in time or a duration in milliseconds, matching upstream's TimePoint.
// Spelled here rather than included from timeman.h so this header stays a leaf.
typedef int64_t LimitsTimePoint;

typedef struct {
    // Point at the caller-owned `go searchmoves` list. Null with a zero count when the
    // command named no moves, which means "search every root move".
    const SearchMoveText *searchmoves;
    size_t searchmoves_count;

    LimitsTimePoint time[COLOR_NB];  // time[WHITE], time[BLACK]
    LimitsTimePoint inc[COLOR_NB];   // inc[WHITE], inc[BLACK]
    LimitsTimePoint npmsec;          // `nodestime`: nodes per millisecond, 0 to disable
    LimitsTimePoint movetime;
    LimitsTimePoint start_time;

    int movestogo;
    int depth;
    int mate;
    int perft;
    int infinite;

    uint64_t nodes;
    bool ponder_mode;
} LimitsType;

// Return a zeroed record, matching upstream's explicit constructor (search.h:163).
static inline LimitsType limits_type_default(void) { return (LimitsType) { 0 }; }

// Report whether the clock governs the search, as upstream's use_time_management does
// (search.h:172): either side's remaining time being non-zero is enough.
static inline bool limits_use_time_management(const LimitsType *limits) {
    return limits->time[WHITE] != 0 || limits->time[BLACK] != 0;
}

// Return the perft depth as a count. Only call this when `perft` is positive.
static inline size_t limits_perft_value(const LimitsType *limits) { return (size_t) limits->perft; }

static inline size_t limits_searchmove_count(const LimitsType *limits) {
    return limits->searchmoves_count;
}

// Return searchmove INDEX, which must be below limits_searchmove_count.
static inline const SearchMoveText *limits_searchmove_at(const LimitsType *limits, size_t index) {
    return &limits->searchmoves[index];
}

#endif  // MCFISH_LIMITS_TYPE_H
