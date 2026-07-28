// Run the main alpha-beta node: Steps 1-21, node init through the TT store.
//
// ONE FUNCTION BODY, as upstream's `search<NodeType>` is one function body. The
// move loop reads roughly thirty values Steps 1-12 established, and it reads them
// as LOCALS in the same scope. A previous shape put Steps 13-21 in a second file
// and passed those values across as a struct; the struct was then built at every
// interior node and read back through a pointer, and its field order acquired a
// comment about the lowering the stores provoked. None of that exists upstream.
// Do not re-split it.
//
// search_node recurses on itself and dives into qsearch_node at depth 0. It never
// calls the iterative-deepening driver.
//
// Golden: `Stockfish/src/search.cpp: search<NodeType>`.

#ifndef MCFISH_SEARCH_MAIN_H
#define MCFISH_SEARCH_MAIN_H

#include "search_types.h"

#include "../board/position.h"
#include "../board/types.h"

// Mirror upstream `template<NodeType> search<Root>/<PV>/<NonPV>(..., bool cutNode)`:
// the node kind is the tag, `cut_node` is a runtime flag.
Value search_node(SearchCtx *ctx,
                  Position *pos,
                  Stack *ss,
                  Value alpha,
                  Value beta,
                  int depth,
                  bool cut_node,
                  NodeType nt);

// Name the two specializations the move loop recurses into, so a call site whose
// NodeType is a literal lands on the matching clone directly — upstream's
// `search<NonPV>(...)` / `search<PV>(...)` are direct calls, and routing them
// through the tag dispatcher above pays a per-call test-and-forward the clang
// inliner declines to fold away.
Value search_node_nonpv(
  SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, int depth, bool cut_node);
Value search_node_pv(
  SearchCtx *ctx, Position *pos, Stack *ss, Value alpha, Value beta, int depth, bool cut_node);

#endif  // MCFISH_SEARCH_MAIN_H
