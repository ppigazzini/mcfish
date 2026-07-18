// Construct the root move list and its Syzygy root ranking.
//
// Build the `go`-path root moves: rank the legal moves (or the caller's
// searchmoves) by DTZ, falling back to WDL, when tablebases are loaded, then hand
// back the RootMove array the search binds to.
//
// The invariant that keeps a default build's node count untouched: with no
// SyzygyPath the probe seam reports zero cardinality, no ranking runs, every
// tb_rank and tb_score stays 0, and the returned list is the move list in
// generator order. Everything tablebase-shaped here is behind that gate.
//
// Pure over position / movegen plus the injected tb_source and option_source
// seams — no thread or worker dependency, so the allocation-failure paths are
// testable in isolation.
//
// Ported from zfish `engine/search/root_move_build.zig`.
// Golden: `Stockfish/src/syzygy/tbprobe.cpp: rank_root_moves` and
// `Stockfish/src/search.cpp: Search::Worker::start_searching`.

#ifndef CCFISH_ROOT_MOVE_BUILD_H
#define CCFISH_ROOT_MOVE_BUILD_H

#include "search_types.h"

#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>

typedef struct {
    RootMove *moves;
    size_t count;
    TbConfig tb_config;
} RootMoveList;

// Build the ranked root move list for POS from MOVES. ROOT_FEN must be POS's own
// FEN — the ranking replays each move from it on a scratch board. Return false on
// allocation failure, leaving OUT untouched.
bool root_moves_build(const Position *pos,
                      const char *root_fen,
                      bool chess960,
                      const Move *moves,
                      size_t count,
                      RootMoveList *out);

void root_moves_free(RootMoveList *list);

#endif  // CCFISH_ROOT_MOVE_BUILD_H
