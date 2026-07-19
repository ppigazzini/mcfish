# Engine: the search

Everything under [`src/engine/search/`](../src/engine/search): iterative deepening,
alpha-beta, quiescence, the staged move picker, the history block, the pruning set,
the transposition table, and time management.

Audience: engine contributors. The board APIs this page calls are in
[01-engine-board.md](01-engine-board.md); the leaf evaluation is in
[03-engine-eval.md](03-engine-eval.md).

## Where this zone stands

Milestone M2 in [PORTING.md](PORTING.md) is this zone. It interlocks with M3, and
both halves have landed: the decomposed node bodies are the live search, and they
evaluate through NNUE with the search's own per-colour optimism. Every make/unmake
is bracketed with `eval_acc_push` / `eval_acc_pop` and the accumulator is reset
once per `go`. See [03-engine-eval.md](03-engine-eval.md) for why that bracket is a
contract rather than an optimisation.

**Wired into the binary** ([`../build.sh`](../build.sh) `SOURCES` and
`ENGINE_SOURCES`) — the whole zone:
[`search.c`](../src/engine/search/search.c),
[`search_setup.c`](../src/engine/search/search_setup.c),
[`search_id.c`](../src/engine/search/search_id.c),
[`search_main.c`](../src/engine/search/search_main.c),
[`search_back.c`](../src/engine/search/search_back.c),
[`search_qsearch.c`](../src/engine/search/search_qsearch.c),
[`search_common.c`](../src/engine/search/search_common.c),
[`search_control.c`](../src/engine/search/search_control.c),
[`search_emit.c`](../src/engine/search/search_emit.c),
[`root_move_build.c`](../src/engine/search/root_move_build.c),
[`uci_wdl.c`](../src/engine/search/uci_wdl.c),
[`movepick.c`](../src/engine/search/movepick.c),
[`history.c`](../src/engine/search/history.c),
[`timeman.c`](../src/engine/search/timeman.c),
[`tt.c`](../src/engine/search/tt.c),
[`search_threads.c`](../src/engine/search/search_threads.c), plus the injection seams
[`pool_source.h`](../src/engine/search/pool_source.h),
[`output_sink.h`](../src/engine/search/output_sink.h),
[`option_source.h`](../src/engine/search/option_source.h),
[`time_source.h`](../src/engine/search/time_source.h) and
[`tb_source.h`](../src/engine/search/tb_source.h).

**`search.c` is a facade, not the search.** It owns the public surface
[`search.h`](../src/engine/search/search.h) declares — `search_go`,
`search_set_output`, `search_stop`, `perft` — and nothing else: it maps the shell's
`SearchLimits` onto the zone's `SearchZoneLimits`, registers the four seams, builds
the root move list, and hands the search to `iterative_deepening`. It must never
re-derive a margin, a reduction or an info line; a facade that computes is a second
search that drifts from the first.

**The search runs on N threads.** `SearchCtx` is the hot per-node context every
node body threads through, and it lives inside a `SearchWorker` — one per thread,
holding that worker's history tables, its NNUE arena and, on thread 0 only, the
`SearchManager`. The driver is
[`search_threads.c`](../src/engine/search/search_threads.c); the pool sum in
`check_time`, `Worker::elapsed`, `output_pv`, the `nodes as time` settle and the
`best_move_changes` collection reach it through
[`pool_source.h`](../src/engine/search/pool_source.h), which answers with thread
0's own values at `Threads 1`. See
[04-multithreading.md](04-multithreading.md).

## Iterative deepening

`search_go(pos, limits)` in [`search.c`](../src/engine/search/search.c) is the
per-`go` prologue and nothing more. It:

1. Registers the four seams, clears the stop / ponder flags, and resets the
   per-game manager scalars upstream resets in `ThreadPool::clear`.
2. Calls `eval_acc_reset()` — **once per `go`, not once per iteration** — so the
   first evaluation refreshes from this board rather than from the previous
   search's diffs, then clears the history block.
3. Generates the legal root moves. With none, it emits the mate / stalemate info
   line plus `bestmove (none)` and returns `mated_in(0)` or `VALUE_DRAW`.
4. Builds the ranked root move list through
   [`root_move_build.c`](../src/engine/search/root_move_build.c). With no
   `SyzygyPath` the probe seam reports zero cardinality, no ranking runs, and the
   list is the move list in generator order.
5. Fills the `SearchCtx` and the time budget through
   [`search_setup.c`](../src/engine/search/search_setup.c) — **once**, before the
   first node, so the recursion never re-derives any of it.
6. Runs `iterative_deepening`, then emits the final PV (if the depth loop did not
   already) and `bestmove`.

The depth loop itself is [`search_id.c`](../src/engine/search/search_id.c): the
`root_depth` walk, the aspiration window, the MultiPV lines, the skill handicap,
the forgotten-mate rollback and the per-iteration time decision.

### The seven sentinel frames

`STACK_PAD` is 7, and `ss` is offset by it so `stack[i].ply == i - STACK_PAD`. That
padding is not slack: the continuation-history walk reads six frames back and the
correction read reaches `(ss - 4)`, so a root node at `stack[0]` would index before
the array. `search_stack_init` gives every sentinel a valid base continuation page
and a valid base correction page — **not** null pointers, because the history code
dereferences those slots unconditionally. Two frames above `MAX_PLY` are padding
too, for the `(ss + 2)` cutoff-count reset.

### The aspiration window

Each MultiPV line re-searches around the previous iteration's average score. On a
fail low the window drops its lower edge and **resets** the fail-high counter; on a
fail high it raises the upper edge and keeps counting, and that counter is what
shortens the re-searched depth. Both edges grow by the same factor per re-search.
Reorder those updates and the node count moves without the move changing.

The window is also where `optimism` is set: `optimism[us]` is derived from the
line's average score, `optimism[~us]` is its negation, and both are read by
`search_evaluate` for the side to move at each node.

### The info line

[`search_emit.c`](../src/engine/search/search_emit.c) formats every line and routes
it through `output_sink`; the field order and the score spelling are
[`uci_wdl.c`](../src/engine/search/uci_wdl.c)'s. It has **no dependency on the
search algorithm**, which is what lets the byte-exactness of the lines be gated
independently of the node count. The `pv` field carries the whole variation, and
`SearchResult::ponder_move` is the second move of it.

## Alpha-beta

`search_node` ([`search_main.c`](../src/engine/search/search_main.c)) runs Steps
1-12 of a node and hands the established state to `search_run_back`
([`search_back.c`](../src/engine/search/search_back.c)), which owns the move loop
and the finalization, Steps 13-21. That pair is **one component with one deliberate
import cycle, and the cycle is the recursion** — `search_run_back` calls back into
`search_node` for every child. Do not break it by inverting an import or threading
a function pointer: it would buy nothing and cost an optimizer barrier on the
hottest path in the engine.

Node order:

1. **Upcoming-repetition draw** (non-root) — the cuckoo test, then Step 1 node
   init, Step 2 the aborted-search / draw / `MAX_PLY` bail, Step 3 mate-distance
   pruning.
2. **Step 4 TT probe**, with the `cut_node == (ttValue >= beta)` gate on the
   cutoff, the deep-TT verification search, and the depth penalty on an entry that
   could not justify its cutoff.
3. **Step 5 static eval** through the correction histories, plus `improving`,
   `opponent_worsening` and the hindsight reduction adjustments.
4. **Step 6 tablebase probe**, gated on `tb_config.cardinality` — zero without a
   `SyzygyPath`, so a default build never enters it.
5. **Step 7 razoring, Step 8 futility, Step 9 null move** and its verification
   search, **Step 10 internal iterative reductions**, **Step 11 ProbCut**, **Step 12
   deep ProbCut**.
6. **Step 13 move loop** with `follow_pv`, **Step 14** move-count / history /
   futility / SEE pruning for quiets and captures, **Step 15 singular extensions**
   with the double and triple margins, **Steps 16-18** the 1024-scaled LMR schedule
   and its re-searches.
7. **Steps 20-21** best-move bookkeeping, the stat updates, the TT store and the
   correction-history nudge.

`NodeType` is a tag, not two booleans: the body derives `pv_node` and `root_node`
from it, so the two cannot drift apart the way two independent flags can.

### The TT move must pass `search_pseudo_legal` first

`MovePicker` returns the TT move **unchecked** — its header says so — and
`pos_legal` is *undefined* on a move that could not have been generated. A 16-bit
key fragment collides eventually, so a stale move from an unrelated position will
reach the picker. Deleting the guard buys nothing and makes the failure a wild
memory read on some future position.

`search_pseudo_legal` and `search_gives_check` are board-zone predicates the search
zone still carries; `src/engine/board/legality.c` holds the canonical
`pos_pseudo_legal`, and both copies go when that module enters the build.
`search_gives_check` additionally recomputes upstream's `StateInfo::checkSquares` on
every call, because mcfish's `StateInfo` does not cache it.

### The TT cutoff

The comparison is in **real depth**, not a biased one: `tt_probe` already added
`DEPTH_ENTRY_OFFSET` back when it built the `TTData`. Inside the PV the cutoff is
suppressed entirely: a bound-based return there would truncate the line the caller
reports.

### Null-move pruning

The non-pawn-material guard is the zugzwang defence: in a king-and-pawns endgame,
passing is often genuinely the best move available, so the "if I do nothing and
still fail high" reasoning is invalid. The verification search past `nmp_min_ply`
is what stops a null-move-only line from proving a cutoff it cannot deliver.

The null move **publishes sentinel history pages** before recursing:
`ss->current_move` becomes `MOVE_NULL` and both continuation pointers become the
base pages. Without that, the six-ply continuation walk would score a phantom move
on this frame.

**Unlike a real move, it is not bracketed by `eval_acc_push` / `eval_acc_pop`**, and
that asymmetry is deliberate rather than an omission: a null move moves no piece, so
the child must evaluate against this node's own accumulator slot. Pushing an empty
diff would not be equivalent, because a pushed diff is applied rather than skipped.
`pos_do_null_move`'s empty delta goes into the position's scratch records and is
discarded. The Step 4 TT verification make/unmake is unbracketed for the same
reason: it evaluates nothing between the two. See
[03-engine-eval.md](03-engine-eval.md).

Null-move pruning is also the reason the evaluation must not be symmetric under a
null move — see [03-engine-eval.md](03-engine-eval.md).

### The continuation index is the piece that LEFT `from`

`search_do_move` reads the moved piece **before** the make, because upstream indexes
the continuation pages by `DirtyPiece::pc`, which `Stockfish/src/position.cpp:848`
fills from `piece_on(from)` ahead of the move. For a promotion that is the pawn, not
the piece standing on `to` afterwards. Reading it post-move is a divergence that
only shows up in promotion-heavy positions, which is exactly where it is hardest to
notice.

### Reductions and margins

Every margin, reduction weight and bonus scale lives in
[`search_common.c`](../src/engine/search/search_common.c), and its header states the
rule that governs all of them: **nothing there is derived, everything is
transcribed.** Each constant is upstream's tuned value and each division truncates
toward zero, matching upstream's C++ integer division exactly. A cleaner
formulation that moves a rounding boundary moves the node count, so a change there
is a behaviour change even when it looks like a simplification.

Where upstream computes in `int` and relies on two's-complement wrap, the port goes
through unsigned arithmetic and casts back, because signed overflow is undefined in
C. Those spots are marked.

### Terminal nodes

`move_count == 0` after the loop means mate if in check, stalemate otherwise. This is
why the legal counter cannot be replaced by "the generator returned nothing": the
generators are pseudo-legal, so a position with pseudo-legal moves and no legal ones
is exactly the mate/stalemate case.

## Quiescence

`qsearch_node` ([`search_qsearch.c`](../src/engine/search/search_qsearch.c)) extends
the leaf until the position is quiet. **It is a call-graph leaf**: it recurses only
into itself, never into `search_node`, which is what keeps the zone's one import
cycle confined to the `search_main` / `search_back` pair.

**Stand-pat**: when not in check, evaluate first — through the correction tables,
like the main node — and treat that score as a floor, since the side to move may
decline every capture. If it already beats beta, return; if it beats alpha, raise
alpha. In check, stand-pat is skipped entirely: declining is not an option and the
static eval is unreliable, so the evasion set is searched from a mated floor.

Move set: the picker is initialised at `DEPTH_QS`, which selects the qsearch stage
chain; being in check overrides it with the evasion chain.

The mate test is conditional on being in check. A quiescence node with no captures
is not terminal; it just has nothing left to try, and returning a mate score there
would invent mates all over the tree.

`search_qsearch.c` also owns the primitives both node bodies share — `pv_update`,
`is_shuffling`, `search_correction_value`, `adjust_key50` — because it is the half
of the pair with no dependency on the other.

## Move ordering

[`movepick.h`](../src/engine/search/movepick.h) /
[`movepick.c`](../src/engine/search/movepick.c).

**The picker is a lazy state machine, not a sorted list.** `movepick_next` walks the
stages in order and generates only when a stage demands it, so a node that fails
high on the TT move never runs the generator at all. The stage constants are
upstream's numbering, and the shape is load-bearing: `initMainStage` computes a
stage by adding 1 when there is no TT move, **so each `*_TT` stage must be
immediately followed by its init stage.** Inserting a stage between them silently
skips generation.

Four chains exist — main, evasion, probcut, qsearch — and each ends by returning
`MOVE_NONE`.

Two invariants a caller must respect:

- **The TT move is returned first and skipped everywhere else**, so the caller sees
  each move exactly once even though the generator also produces it.
- **`tt_move` must already be known pseudo-legal**; the picker does not check it.
  Pass `MOVE_NONE` when there is none.

The move buffer is reused across stages: quiets are scored on top of the captures
already consumed, so `cur` only moves forward and one `MAX_MOVES` buffer covers both
lists.

`see_ge` lives in `movepick.c` and is what splits good captures from bad. Upstream
has it on `Position`; mcfish has both — `pos_see_ge` in the board zone and the
picker's own — and collapsing them is board-zone work, not search work.

## History

[`history.h`](../src/engine/search/history.h) /
[`history.c`](../src/engine/search/history.c) own the ordering tables, split the way
upstream splits them. `Histories` is what ONE worker owns and writes without
synchronisation — butterfly (main), low-ply, capture, continuation-correction and the
tt-move counter — plus a pointer to the `SharedHistories` bank its NUMA node shares:
the two key-indexed tables (pawn, correction) and the continuation block
(upstream `history.h:202`, `search.h:341`).

The bank's key-indexed tables are **sized by the node's thread count**, as upstream's
`DynStats` sizes them, so the index masks a one-thread run takes are upstream's
one-thread masks. `shared_histories_create` is the only writer of a size and its mask
together — binding one without the other turns a wrapped index into an out-of-range
read.

`history_clear` is upstream's `Worker::clear`: it fills this worker's own tables, fills
the whole shared continuation block (upstream has every worker fill all of it), and
clears only **its stripe** `[i * n / total, (i + 1) * n / total)` of the two
key-indexed tables. With one worker the stripe is the whole table, which is why the
single-threaded clear is unchanged by the split. The fill constants
`CORRECTION_HISTORY_FILL` and `PAWN_HISTORY_FILL` are declared once in `history.h`:
**neither is zero**, so any clear that reaches for `memset` writes the wrong table.

**The main and low-ply tables are indexed by the raw 16-bit move word**, one row per
colour (or per low ply). That is why the move encoding in
[01-engine-board.md](01-engine-board.md) is not free to change: it sizes these
tables.

**Every entry is a gravity-bounded `int16_t`, and that is the invariant the code
cannot show.** `stats_update` never lets `|entry|` exceed the `D` it is called with:

```c
// entry += bonus - entry * |bonus| / D
```

The narrowing to `int16_t` at the end is therefore lossless for every `D` used here
(the largest is 30000). **Raise a `D` above 32767 and the narrowing wraps
silently** — a deep search would start reading a sign-flipped ordering score, and
no gate would fail. Delete the decay term entirely and the addition itself becomes
signed overflow, which is undefined behaviour that UBSan in `./build.sh test` sees
only if a test searches deep enough to reach it.

`history.c` must not depend on `search.c`'s `Stack` layout, so the caller gathers
what each update needs into a `HistoryStack` first: `frames[k]` is `(ss - 1 - k)`,
so `frames` is the walk from `ss` and `frames + 1` is the walk from `ss - 1`. Get
that offset wrong and the update credits the wrong ply.

The block is cleared per `go`. Upstream clears it on `ucinewgame` instead; the live
UCI layer has no hook for that, which is a shell gap — see
[07-shell.md](07-shell.md).

## The transposition table

[`tt.h`](../src/engine/search/tt.h) / [`tt.c`](../src/engine/search/tt.c), with the
storage types split into
[`../src/engine/state/tt_types.h`](../src/engine/state/tt_types.h) so the state zone
can type a worker's `tt` reference without pulling in the probe path.

This is upstream's table, ported faithfully. The layout:

- **A cluster of three entries**, `TT_CLUSTER_SIZE == 3`, padded to exactly 32
  bytes so a cluster never straddles a 64-byte line in an aligned table.
- **An entry is ten bytes**: `key16`, `depth8`, `gen_bound8`, `move16`, `value16`,
  `eval16` — in the order `tt_probe` reads them, because memory is fastest
  sequentially.
- **Indexing is `mul_hi64(key, cluster_count)`**: the high 64 bits of the 128-bit
  product, which maps a key onto the cluster range with no modulo and no
  power-of-two constraint on the size. The **low** 16 bits of the same key become
  `key16`, the in-cluster verification tag. The two ranges do not overlap, so the
  tag carries real information rather than re-confirming the index.
- `tt_resize` allocates 64-byte aligned through `aligned_alloc`, rounding the byte
  count up to a whole number of cache lines because `aligned_alloc` requires a size
  that is a multiple of its alignment.

**The table is lossy on purpose.** A probe may miss a stored position, a store may
be declined, and a store may evict a deeper entry. Nothing in the search may depend
on a probe hitting: correct play must survive the table being empty. `test_search`
in [`../tests/test_main.c`](../tests/test_main.c) asserts one instance of this by
clearing the table and re-finding a mate in one.

Reads and writes are non-atomic and may race once M4 lands. A `TTData` copy may
therefore be self-inconsistent — but it is a *copy*, and it does not change under
the caller once taken. That is the property `tt_probe` is built to give.

### depth8, the occupancy test, and DEPTH_ENTRY_OFFSET

**`depth8 == 0` is the occupancy test.** There is no separate "used" flag, and
`tt_clear` zeroes the array, so a slot with `depth8 == 0` is by definition empty.
That is why every stored depth is biased:

```c
enum : int32_t { DEPTH_ENTRY_OFFSET = -3 };

writer->depth8 = (uint8_t) (d - DEPTH_ENTRY_OFFSET);   // store
.depth = DEPTH_ENTRY_OFFSET + (int32_t) entry->depth8; // read back
```

The offset is negative, so the bias is an addition of 3 and an empty entry reads
back as depth `DEPTH_ENTRY_OFFSET`. `empty_data()` returns exactly that, so a miss
and an occupied depth-`DEPTH_ENTRY_OFFSET` entry are indistinguishable to a caller
by design.

The corollary for any depth decrement: **it must saturate at 0, not wrap.**
`depth_saturating_sub` is upstream's `std::max(int(depth8) - n, 0)`, and its comment
names the reason — a wrapping subtract would turn the shallowest entry into the
deepest one in the table, and the search would trust it. `tt_penalize` is the
public form of that operation.

### gen_bound8 packs three fields

```
bit 7      : is-PV
bits 6..5  : Bound
bits 4..0  : generation
```

The generation takes the **low** bits so a wrapping increment never disturbs the two
above it, and `tt_new_search()` masks with `GENERATION_MASK` for the same reason.

`entry_relative_age` counts generations the way clocks count hours — `0 - 1 == 31` —
and the subtraction is **unsigned** so it borrows correctly regardless of the pv and
bound bits sitting above the field. Signed arithmetic there would be undefined on
overflow and would not borrow.

### Replacement and aging

On a **miss**, `tt_probe` returns a writer to the least valuable entry in the
cluster, where value is `depth8 - 8 * relative_age`. Age is worth eight plies of
depth: that ratio is what stops the table filling with deep entries from an earlier
`go` that the current search can never evict.

`tt_save` writes through that writer, and it declines to write:

```c
if (b == BOUND_EXACT || key16 != writer->key16
    || d - DEPTH_ENTRY_OFFSET + 2 * (int32_t) pv > (int32_t) writer->depth8 - 4
    || entry_relative_age(writer, TT.generation8) != 0) { ...overwrite... }
```

An exact bound always wins, a different position always wins, a stale entry always
loses, and otherwise the new depth must beat the resident one — with a two-ply
bonus for a PV entry and a four-ply grace for the resident.

**The move field is preserved separately**, before that test: a store with no move
keeps the resident move when the key matches, because a stale move from the same
position still orders better than nothing.

**Secondary aging** is the `else` branch, and it matters for elementary mate
finding: an entry this store did *not* overwrite loses one ply of depth if it is at
least 5 plies deep, non-exact, and holds a decisive value whose magnitude is below
`VALUE_INFINITE`. Both halves of that inner test are upstream's and are not the
same condition — the magnitude check excludes an entry holding `±VALUE_INFINITE`
outright.

### Mate score re-basing

A mate score is stored **relative to the node where it was found**, and read back
relative to the node probing it:

```c
value_to_tt(v, ply)   // v + ply for mate scores, v - ply for mated scores
value_from_tt(v, ply) // the inverse, and VALUE_NONE passes through untouched
```

Without this, a mate-in-3 stored at ply 6 and probed at ply 2 would be reported as a
mate at the wrong distance, and the engine would either delay a mate or claim one it
cannot reach. `test_tt` asserts the round-trip across a range of plies and that a
plain centipawn score is unaffected.

### hashfull

`tt_hashfull(max_age)` samples the **first 1000 clusters**, counts occupied entries
no older than `max_age` generations, and divides by the cluster size to report
permille. It is a UCI display value only; a full scan would be O(table) on every
info line.

### The is-PV bit round-trips

`ss->tt_pv` is propagated down the tree from the PV and from probed entries, stored
on every `tt_save` and read back by `search_tt_probe`. It steers the futility gate,
the singular margins and the LMR schedule, and it is worth two plies of depth in the
replacement race — so a build where it were pinned to `false` would prune a
different tree while still playing plausible chess.

## Time management and determinism

[`timeman.h`](../src/engine/search/timeman.h) /
[`timeman.c`](../src/engine/search/timeman.c) own the budget.

**The budget arithmetic is a pure function of its input record.**
`timeman_compute` reads no clock, no option and no position, which is what makes
the formulas testable without a running search. `timeman_init` is the stateful
wrapper that stores the result; `search_go` calls it once, before the recursion
starts, and the recursion never re-derives it. If it did, the node count would
become a function of *when* the clock was read.

`available_nodes` is `-1` until the first init of a `nodes as time` game and is
carried across moves; `timeman_clear` resets it at the start of a new game.

### Why the clock is read on a call counter

`check_time` ([`search_control.c`](../src/engine/search/search_control.c)) decrements
a counter on every node and reads the clock only when it reaches zero, then reloads
it — from the node limit when there is one, so `go nodes N` cannot overshoot by more
than a checkpoint. Two reasons, and the second is the load-bearing one:

- **Cost.** `clock_gettime` per node would be a meaningful fraction of the cost of a
  node.
- **Determinism.** The search must visit the same nodes in the same order on every
  run at a fixed depth from a fixed TT state. That is what
  [`../tools/signature.golden`](../tools/signature.golden) pins and what
  `./build.sh signature` asserts. A time check that fired on wall-clock alone would
  make the node total a function of machine speed and load, and the anchor would
  drift on a busy CI runner instead of on a real behaviour change.

`timeman_elapsed_time` is the single clock read in the module, and the *caller*
decides how often to make it — which is why the search keeps its elapsed queries on
a node-count checkpoint and out of the recursion. In `nodes as time` mode
`timeman_elapsed` reads no clock at all.

Under a fixed-depth limit — which is what bench and the goldens use — no limit in
`SearchTimeState` is armed, so nothing the clock returns can change a decision. That
is the property that makes the signature reproducible across machines.

`test_search` asserts determinism directly: same position, same cleared table, two
searches, equal node counts.

Anything that makes node counts depend on wall-clock time breaks the gate rather
than merely slowing the engine down. Before adding a time-dependent decision inside
the recursion, run:

```bash
./build.sh signature     # must stay green
./build.sh test          # the determinism check lives here
```

### Stopping

`search_stop()` sets the `atomic_bool` the whole zone reads through
`search_stopped`. Note the limit: the UCI loop is single-threaded and reads stdin
only between commands, so **while a search runs there is nothing reading input to
call it**. In practice the flag is set by `quit` or `stop` arriving before or after a
search, and by `check_time` itself on deadline. A `go infinite` with no way to
interrupt it does not return. See [07-shell.md](07-shell.md).

An aborted iteration is discarded by the depth loop rather than by the node bodies,
and the abort paths in `search_id.c` are where the care is: a proven-loss score from
an aborted search is rolled back to the previous iteration's, an aborted MultiPV line
cannot overtake a completed one, and a mate found in an earlier iteration is never
replaced by a later iteration that failed to re-find it.

## What is still not upstream's

[`../src/engine/search/PORT_NOTES_search_zone.md`](../src/engine/search/PORT_NOTES_search_zone.md)
is the file-by-file map. What the live zone does not yet do:

- **Threads.** `check_time` gates on `ctx->nodes`, which *is* the pool count at one
  worker. The pool sum, `increase_depth` across workers, thread voting and
  `best_move_changes` aggregation are single-worker shapes. See
  [06-platform.md](06-platform.md).
- **The option model is installed, and its fallback is still a trap.** `uci_loop`
  registers the shell's table behind the `option_source` seam with
  `search_set_option_source`, so MultiPV, Skill Level, UCI_Elo, Move Overhead,
  nodestime, Ponder and UCI_ShowWDL reach the search from the table the handshake
  renders. A caller that installs nothing — the bench harness, the unit tests —
  falls back to `facade_option_int`, which answers upstream's defaults. That
  fallback is **not** neutral: the zone's own default answers 0 to everything,
  which reads as MultiPV 0 and Skill Level 0, wrong searches rather than absent
  ones. The four Syzygy reads are deliberately left unregistered while no prober
  exists. See [07-shell.md](07-shell.md).
- **Per-game manager state.** `best_previous_score`, `best_previous_average_score`
  and `previous_time_reduction` are reset per `go` rather than carried between moves,
  alongside the history block. Upstream resets them on `ucinewgame`; the live UCI
  layer has no hook for that, which is a shell gap.
- **`pos_non_pawn_material` is a function, not cached state.** Upstream reads
  `st->non_pawn_material[c]`. Step 14 calls it twice per move; caching it on
  `StateInfo` is a pure win with no behaviour change.
- **Tablebases.** Every tablebase path is behind `tb_config.cardinality`, which is
  zero with no prober attached — the correct answer, not a degraded one. See
  [06-platform.md](06-platform.md).

The zone is close enough to upstream that a per-position differential at fixed depth
is now the useful gate, not a proxy: run it before and after any change here, and
treat a position that moves as a finding rather than as noise.

## perft

`perft(pos, depth, root)` lives here rather than in `movegen.c` because it shares
the output sink. It generates fully legal moves at every node and bulk-counts at
depth 1 **off the root only**: the root still walks each move so the per-move split
it prints stays comparable with upstream's `go perft`. The counts it produces are
gated two ways — by `test_perft` in the unit suite and by
[`../tools/perft.table`](../tools/perft.table) via `./build.sh perft`. See
[09-tooling-ci.md](09-tooling-ci.md).
