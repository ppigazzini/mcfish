// Report search progress over UCI: the info / bestmove emission and the MultiPV
// walk.
//
// This is the output half of the search driver — everything that formats and
// prints a line during a search. It reads the SearchCtx and the options and
// routes text through output_sink; it has NO dependency on the search algorithm
// itself, which is what lets the byte-exactness of the lines be gated
// independently of the node count.
//
// Ported from zfish `engine/search/search_emit.zig`.
// Golden: `Stockfish/src/engine.cpp` / `search.cpp` (the info-line assembly).

#ifndef MCFISH_SEARCH_EMIT_H
#define MCFISH_SEARCH_EMIT_H

#include "search_types.h"

#include "../board/position.h"
#include "../board/types.h"

// Emit one info line per MultiPV line at DEPTH.
void search_emit_pv(SearchCtx *ctx, int depth);

// Emit "info depth 0 score ..." + "bestmove (none)" for a checkmated or
// stalemated root.
void search_emit_no_moves(const Position *pos);

// Emit "bestmove X[ ponder Y]" from BEST's PV.
void search_emit_bestmove(const Position *pos, const RootMove *best);

// Emit "info depth D currmove M currmovenumber N" from inside the root move loop.
void search_emit_root_on_iter(const SearchCtx *ctx, int depth, Move move, int move_count);

#endif  // MCFISH_SEARCH_EMIT_H
