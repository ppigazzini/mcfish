// Run the main alpha-beta node: Steps 1-12, up to the move loop.
//
// COMPONENT: search_main and search_back are ONE component, deliberately.
// They form the search zone's only import cycle (search_node <-> search_run_back)
// and that cycle IS the alpha-beta recursion, not a layering defect. search_node
// runs a node's Steps 1-12, hands the node state to search_back's move loop
// (Steps 13-21), and that loop recurses back into search_node for each child.
// Splitting the file did not split the recursion, so do not "fix" this by
// inverting an import or threading a function pointer: it would buy nothing and
// cost an optimizer barrier on the hottest path in the engine.
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
