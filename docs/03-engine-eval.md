# Engine: the evaluation

Everything under [`src/engine/eval/`](../src/engine/eval): the static evaluation the
search calls at every leaf, the NNUE that produces it, and the classical term that
stands in when no net is resident.

Audience: engine contributors. The search's use of it — stand-pat, reverse futility,
null-move pruning — is in [02-engine-search.md](02-engine-search.md).

## The shape of this zone

[`evaluate.h`](../src/engine/eval/evaluate.h) states the invariant the whole page
turns on:

> **`evaluate` returns the NNUE score whenever a net is resident.** The classical
> material + piece-square term below it is a fallback for a netless run only.

`evaluate` is one branch on `NetLoaded`. With a net, it runs the forward pass and
scales the result; without one, it returns `evaluate_classical`. Both entry points —
`evaluate` and `evaluate_trace` — are side-relative, centipawn-scaled and free of
side effects, so the search never knows which path ran.

**The classical term is still scaffolding to be deleted, and it is not a second
evaluation to tune.** Do not extend it, do not give it callers NNUE will not
satisfy, and do not add a game-phase term to it. It exists so a build with no `.nnue`
file on disk still plays legal chess and still answers `eval`, which is what keeps
`bench` runnable on a machine that has not downloaded a net.

The NNUE modules and `evaluate.c` are all in [`../build.sh`](../build.sh)'s
`SOURCES` and `ENGINE_SOURCES`, so `./build.sh build`, `test`, `zone-check` and the
gate battery cover them.

| Module | Owns |
| --- | --- |
| [`evaluate.c`](../src/engine/eval/evaluate.c) | the dispatch, the NNUE runtime state, the arena ownership, the scaling, the traces, the classical fallback |
| [`network.c`](../src/engine/eval/nnue/network.c) | the net object, the load path, the verify report, the forward-pass entry |
| [`nnue_parse.c`](../src/engine/eval/nnue/nnue_parse.c) | the `.nnue` parse primitives and the transformer's in-memory byte layout |
| [`nnue_weight_storage.c`](../src/engine/eval/nnue/nnue_weight_storage.c) | the weight buffers and the loaded-net identity |
| [`nnue_hash.c`](../src/engine/eval/nnue/nnue_hash.c) | the architecture hashes and the MurmurHash2-64A content hashes |
| [`nnue_ft.c`](../src/engine/eval/nnue/nnue_ft.c) | the feature-transformer blob layout and its typed accessors |
| [`nnue_feature.c`](../src/engine/eval/nnue/nnue_feature.c), [`nnue_feature_bb.c`](../src/engine/eval/nnue/nnue_feature_bb.c) | the `HalfKAv2_hm` and `full_threats` index producers |
| [`nnue_accumulator.c`](../src/engine/eval/nnue/nnue_accumulator.c) | the per-ply accumulator stack, the refresh cache, the transform to the first layer's input |
| [`nnue_affine.c`](../src/engine/eval/nnue/nnue_affine.c) | the affine kernel and the two activations |
| [`nnue_inference.c`](../src/engine/eval/nnue/nnue_inference.c) | the per-bucket forward pass |
| [`simd.h`](../src/engine/eval/nnue/simd.h) | the vector vocabulary, in two implementations |
| [`nnue_architecture.h`](../src/engine/eval/nnue/nnue_architecture.h) | the SFNNv15 dimensions |

Upstream's `nnue/` sources are the golden.

## Startup, loading, and the netless run

Three things happen in three different places, and the split is deliberate.

**`eval_nnue_init()` runs from [`../src/shell/main.c`](../src/shell/main.c)**, in
the same phase as `bitboards_init` and `attacks_init`. It builds the feature index
tables and allocates the two arenas. The ordering is load-bearing for the same
reason as the attack tables: **the feature tables are zero, not garbage, before the
call**, so a missing `nnue_feature_init` is a silent all-zero feature set rather
than a crash.

**The net is loaded by the shell**, because the shell owns the `EvalFile` option.
`eval_nnue_load` searches `"<internal>"`, the working directory, then the root
directory the binary was launched from — which must carry its trailing separator,
because the concatenation inserts none. The net is a runtime input, never embedded;
it is downloaded, not committed.

This repository keeps it in `resources/`, beside the Syzygy tables, and every
`./build.sh` step that runs the engine runs it **from** `resources/` — so the file
is found through the second candidate, the working directory. The search order
itself is upstream's and gains no fourth entry for it.

`./build.sh net` names the file this build expects, lists those three directories,
and prints the download command. It deliberately does **not** fetch: the net is not
a build product, and a build step that downloads it makes every clean build a
network dependency.

**A failed load is not fatal, and that is a deliberate divergence from upstream.**
Upstream's `Network::verify` calls `exit(EXIT_FAILURE)` on a net it could not load.
mcfish reports the failure, leaves `NetLoaded` false, and keeps playing on the
classical fallback. That choice is why `evaluate` needs the branch at all, and it is
why the status line matters:

```c
const char *eval_nnue_status(void);
```

`uci.c` prints it through `info string` at the four sites upstream prints it, so a
user can always tell which evaluation produced a number. **A silent fallback would
be the worse bug**: a bench run on a machine with no net would look like a
strength regression rather than a missing file.

`eval_nnue_available()` reports which path is live. Nothing in the search calls it —
the search does not need to know — but a harness comparing against upstream must.

## The accumulator bracket

**The accumulator is incremental, and the diffs are not optional bookkeeping.**
Slot `i` of the stack holds the accumulator after `i` plies *and* the diff that
produced it; evaluation walks from the nearest computed slot to the top applying
diffs, falling back to a full refresh only when a king move invalidated the bucketed
features.

That imposes a contract on every caller that moves a piece:

```c
eval_acc_push(&dp, &dts);              // hands back the arena's own slots
pos_do_move(pos, m, &st, false, dp, dts);
...
pos_undo_move(pos, m);
eval_acc_pop();
```

[`../src/engine/search/search.c`](../src/engine/search/search.c) brackets both of
its move-making sites this way — the main move loop and the qsearch loop — and calls
`eval_acc_reset()` once per `go`, not once per iteration, so the first evaluation of
a search refreshes from the board rather than from the previous search's diffs.

`perft` is deliberately **not** bracketed. It makes and unmakes moves but never
evaluates, so pushing a slot per node would be pure work; it keeps writing its
deltas into `Position`'s scratch records.

**A missing bracket does not crash. It silently evaluates a different position than
the board holds**, and nothing in the gate battery distinguishes that from a
legitimate node-count change. That is the failure mode to write against when adding
a new call site.

Two details that make the contract cheap to honour:

- **`eval_acc_push` always hands back writable records.** Without a net, or when the
  stack is at its bound, it returns private scratch storage instead — so a call site
  never needs a branch and `pos_do_move`'s out-parameters are never null.
- **The null move is deliberately NOT bracketed.** A null move moves no piece, so
  the child evaluates against the parent's own slot — which is what upstream does.
  Pushing a slot carrying `pos_do_null_move`'s empty delta is *not* equivalent: the
  empty diff is applied rather than skipped, and the resulting accumulator disagrees
  with a full refresh of the same position. That divergence is invisible to perft
  and to the unit suite; it shows up only as evaluations that differ from a
  refresh-per-node control run.

`AccDepth` counts plies above the root so a push can never run past the arena. The
search's `ply >= MAX_PLY` guard already bounds it; the check exists so a future
caller that loses that guard degrades to a stale evaluation rather than writing
outside the arena.

### The two records are byte-identical by contract

The board zone writes `DirtyPiece` and `DirtyThreats`; the accumulator reads the
same bytes back as `NnueDirtyPiece` and `NnueDirtyThreats`. They are **the same
memory**, not a copy — `eval_acc_push` hands the make-move a pointer straight into
the accumulator's arena.

`evaluate.c` pins that with a block of `static_assert`s over the sizes, the
alignment and every field offset the decoders address. Those assertions are the only
thing standing between a field reordering in
[`../src/engine/board/types.h`](../src/engine/board/types.h) and an accumulator that
decodes garbage, so **add an assertion when you add a field.**
[`../src/engine/eval/nnue/PORT_NOTES_accumulator.md`](../src/engine/eval/nnue/PORT_NOTES_accumulator.md)
records the full contract.

## The properties that make the network correct

Each is stated as an invariant in the owning header, and each has a silent failure
mode:

- **The scalar path must be bit-identical to the vector path.** `simd.h` provides
  one vocabulary in two implementations: compiler vector extensions
  (`vector_size`) and a lane-loop fallback. Every operation in it is element-wise
  and total — lane `i` of the result depends only on lane `i` of the operands, by
  the same per-lane C expression in both bodies, which sit adjacent under a single
  `#if` so they are read together. Nothing there reduces, reassociates, rounds,
  saturates or reorders lanes; the only horizontal step any kernel takes is ordinary
  scalar C outside the header and is shared verbatim. **A machine without the ISA
  must produce the same node count**, which is exactly what the gcc lane in
  [09-tooling-ci.md](09-tooling-ci.md) exists to catch.
- **The affine accumulation is exact int32 with no rounding and no overflow**,
  because inputs are bounded by 127 and weights by 128. Integer addition therefore
  commutes, which is what lets the kernel accumulate in the interleaved `OUT*4`
  domain and fold each output's four sublanes together at the end — and that
  reassociation is precisely what keeps the vector and scalar paths equal.
- **The excluded threat pairs are excluded by an out-of-range index, not by an
  error.** `nnue_full_make_index` returns a value `>= NNUE_FULL_DIMENSIONS` for a
  pair the feature set does not encode, and the caller drops it. That return *is*
  the exclusion mechanism, and it is what upstream does. Treating it as a failure,
  or "fixing" it by clamping, changes the feature set.
- **The architecture hash is the file's admission test.**
  [`nnue_architecture.h`](../src/engine/eval/nnue/nnue_architecture.h)'s dimensions
  are the architecture, not a tuning knob: the `.nnue` header commits to them, so a
  net of a different shape is rejected at load rather than mis-parsed.
- **The weight blob's offsets have one definition.**
  [`nnue_ft.h`](../src/engine/eval/nnue/nnue_ft.h) is the sole owner of where each
  region starts. The blob stays a raw byte arena because its shape is fixed by the
  file and by SIMD access, but the handle is a distinct incomplete type rather than
  `void *`, so an accumulator or refresh-cache arena cannot be passed where a
  transformer is wanted.

## Scaling the network output

`nnue_scaled_value` blends the network's `psqt` and `positional` terms with
optimism and material, then damps for the halfmove clock.

Two things in it are load-bearing for bit-exactness and are easy to "clean up" into
a different number: **every divide truncates**, and **every intermediate is widened
to `int64` before the multiply.** Golden is upstream `evaluate.cpp`.

**Optimism arrives from the search.** `evaluate_with_optimism` is the form the
search calls, passing `ctx->optimism[stm]` — the aspiration loop's per-colour bias
for the side to move at that node. Plain `evaluate` passes 0, which is upstream's
own value at the `eval` command and in the trace. The classical placeholder produces
neither network half, so there is nothing for optimism to scale against and it takes
the fallback path unchanged.

The result is bounded away from the tablebase range. The bound is derived from
`VALUE_MATE` and `MAX_PLY` rather than pinned as a literal, so it stays correct
whatever the TB band is — see [06-platform.md](06-platform.md).

### Range and the mate window

`evaluate` must never return a value in the mate range, on either path. The search
relies on it: `VALUE_MATE_IN_MAX_PLY` and `VALUE_MATED_IN_MAX_PLY` are how the
reverse-futility guard, the null-move return check, and the TT re-basing tell a mate
from a positional score. A static eval that reached the mate window would be re-based
as a mate on TT store and reported as a mate that does not exist.

## The classical fallback

`evaluate_classical` sums material and one signed byte per square from `PSQT`, adds a
bishop-pair bonus, and adds a tempo term.

### Material

`piece_value(pt)` in [`../src/engine/board/types.h`](../src/engine/board/types.h)
supplies the base values, and `evaluate_side` multiplies each by the piece count.
The king contributes nothing — `piece_value(KING)` is 0 — because both sides always
have exactly one and the term would cancel.

The same constants back `pos_non_pawn_material` in
[`../src/engine/board/position.c`](../src/engine/board/position.c), which the search
reads as its zugzwang guard before null-move pruning. **Those constants are shared,
so they are not part of what gets deleted with the fallback.**

### Orientation is the error-prone line

```c
v += PSQT[pt][c == WHITE ? s : flip_rank(s)];
```

White reads the square directly; Black reads `flip_rank(s)`, which is `s ^ SQ_A8` —
the vertical mirror. One table serves both colours.

Mirror it wrongly and the failure is not a crash: Black's pawns are rewarded for
retreating while White's are rewarded for advancing. The engine still plays legal
chess, still passes perft, and simply plays one side badly. The guard is
`test_evaluate` in [`../tests/test_main.c`](../tests/test_main.c), which asserts the
start position evaluates identically with White or Black to move.

The mirror is vertical only. There is no horizontal flip, so a table that is not
left-right symmetric encodes a genuine file preference; the king table's back-rank
corners are the deliberate case.

The king table has no game-phase interpolation, so its back-rank preference applies
in the endgame too, where it is wrong. **Do not add a phase term**: the network has
no concept of one, so a phase interpolation added here is a term the fallback's
deletion has to remove anyway.

### Why the tempo bonus is not cosmetic

`+28`, added *after* the side-relative difference, so it does **not** cancel. It is
the reason the classical path is not symmetric under a null move.

Consider a perfectly symmetric evaluation `e`. From a position, `e = 0`. Make a null
move; the side to move flips, the material difference is unchanged, and `e` is still
0. The null-move search then reports "even after giving my opponent a free move I am
not worse" — from a term that carries no information about the free move at all.
The tempo bonus makes moving worth something, so passing costs something.

Two constraints follow: **the term must stay outside `evaluate_side`**, or it
cancels and the symmetry returns silently; and **it must stay small** relative to
the pruning margins in `search.c`. It is a perturbation, not a positional term.

NNUE is not symmetric under a null move either, so the property survives the
fallback's removal — but the constant must come out in the same commit the fallback
does.

`test_evaluate` pins the magnitude loosely — the start position must evaluate above
0 and below 100 — which catches both the term disappearing and the term being scaled
into something that distorts play.

## The eval trace

`evaluate_trace` branches the same way `evaluate` does.

`trace_nnue` prints the per-bucket material/positional split, marks the bucket this
position actually selects, and follows with the three summary lines. It matches
upstream byte for byte.

Every figure in the table goes through `uci_wdl_to_cp` before it is printed. That
is what the header's "(Normalized, ...)" means, and it is the whole of what the
block used to get wrong: the earlier version divided the raw internal value by 100
under that same header, which misreported every cell by the win-rate `a` factor for
the position's material — at the start position, -2.56 where upstream prints -0.67.

`format_cp_aligned_dot` reads the sign off the **raw internal value**, not off the
normalized pawn figure. A small negative value that normalizes to a rounded 0.00
still prints `-`; taking the sign from the rounded double would print `+`.

In check there is no static evaluation to decompose, so `evaluate_trace` returns
`Final evaluation: none (in check)` and nothing else. That string carries one
trailing newline where the NNUE block carries two — upstream's `sync_endl` supplies
the only newline for the in-check string, and a second one for the block, which
already ends in a newline of its own.

`trace_classical` prints one row labelled **Material**, whose value is
`evaluate_side` in full — material *plus* PSQT *plus* the bishop pair. The label is
narrower than the content; fix the label if it misleads, but do not split the terms
into rows. That layout dies with the fallback.

`trace_nnue` calls `eval_acc_reset()` before each of its passes, because a
standalone `eval` must refresh from the board rather than inherit a search's diffs.

The trace's exact bytes are pinned by the `eval` golden, which is derived **from
the oracle** — see [`../tools/cases/eval.uci`](../tools/cases/eval.uci),
[`../tools/GOLDEN_PROVENANCE.md`](../tools/GOLDEN_PROVENANCE.md) and
[09-tooling-ci.md](09-tooling-ci.md). Because it is oracle-derived, regenerating it
from mcfish would convert a red gate into a recorded bug; regenerate it from the
oracle or not at all.

## What is still missing here

- **The classical fallback is still in the tree**, and its deletion waits on the
  point where a netless run is no longer something mcfish needs to support.
- **Bit-exactness with upstream is anchored, not yet proven end to end.** The
  evaluation feeds the ported node bodies (`search.c` is only the facade), and the
  count in [`../tools/signature.golden`](../tools/signature.golden) now equals
  upstream's own `Bench:` at the SHA in
  [`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE) — the target,
  not merely a current total. What the anchor cannot prove is faithfulness off the
  fixed bench list; that is the per-position differential's job. See
  [09-tooling-ci.md](09-tooling-ci.md).
