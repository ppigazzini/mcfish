# Porting mcfish to bit-exactness

mcfish's goal is a **bit-exact 1:1 clone of Stockfish in C23**: the same `bench`
node count, the same bestmove, the same NNUE evaluation, the same Syzygy probing,
the same Lazy-SMP threading. Anything less is an unfinished port, not a variant.

This page is the sequence. `tools/upstream/port_map.tsv` is the module-by-module
work list; run `./build.sh port-status` for the live counts.

## Port from zfish, not from Stockfish

`../zfish` is a complete, bit-exact **Zig** port of Stockfish. Port from it.

Stockfish's C++ uses templates, classes, RAII, operator overloading, and
exceptions — none of which map onto C23 directly, and each of which is a design
decision to be re-made when translating. zfish has already made every one of those
decisions, and its result is **proven bit-exact against upstream**. Its modules are
small, single-responsibility, and written in a language with no hidden control
flow. Translating that into C23 is close to mechanical.

So each row of the port map reads *zfish module → mcfish module*, with the
Stockfish file named as the **golden**:

| Role | Repo | Use |
|---|---|---|
| Port source | `../zfish` | the code you translate, module for module |
| Golden | `../Stockfish` | the definition of correct behaviour |

**Where zfish and Stockfish disagree, Stockfish wins.** The differential gate
compares mcfish against a pristine upstream build, never against zfish. A zfish
divergence found this way is a bug in zfish and should be reported there.

## The anchor

Bit-exactness is a single, brutal, binary check: `bench` must print upstream's node
count for the pinned commit.

- `tools/upstream/UPSTREAM_BASE` pins the Stockfish SHA being ported to.
- Upstream's own `Bench:` for that commit is the target number.
- `tools/signature.golden` holds mcfish's **current** count, which is *not* the
  target yet. It exists so a refactor cannot silently change behaviour mid-port.

**Do not confuse the two.** The signature golden stops accidental drift today; the
upstream bench number is the finish line. Both are numbers a gate computes, so
neither is ever written into prose here.

## Milestones

Each milestone ends at a gate. A milestone is not done until its gate is green.

### M1 — Board fidelity

Bring the board zone to upstream behaviour. Magic bitboards, the full `Position`
state including the threat deltas that the NNUE feature set reads, Chess960, and
the complete FEN surface.

The slider lookup, the Zobrist draw order and the threat deltas are in the binary.
What is left here is the module split: `fen`, `fen_parse`, `legality`, `zobrist`,
`position_query`, `position_snapshot`, `state_list` and `score` are written and
outside `SOURCES`, and several of them define symbols `position.c` still also
defines — so each one enters the build in the same commit that deletes its copy.
See [01-engine-board.md](01-engine-board.md).

**Gate:** perft matches on the reference set *and* on a randomised
position sweep against the upstream binary.

### M2 — Search fidelity

Port the search zone module for module from zfish. This is the largest single
block of behaviour and the one most sensitive to detail — an off-by-one in a
reduction table moves the node count without changing the move.

The whole zone is ported and wired: the decomposed node bodies, the aspiration
window, MultiPV, the emit path, `movepick`, `history`, `timeman` and upstream's
cluster transposition table. `search.c` is a facade over them. What remains is
threads, the option model and the per-game manager state — see the list in
[02-engine-search.md](02-engine-search.md).

**Gate:** node-for-node agreement with upstream at fixed depth on the bench set,
with the classical eval swapped out (see M3 — in practice M2 and M3 interlock, so
expect to land them together and gate them together).

### M3 — NNUE

The big one. `network`, the feature transformer, the incremental accumulator, the
affine layers, the feature sets (`half_ka_v2_hm`, `full_threats`), and the net
loader. The SIMD is written as compiler vector extensions with a lane-loop
fallback under one `#if`. **The scalar path must be bit-identical to the vector
path** — that is the property the whole evaluation rests on.

The net is a runtime input, not embedded. It is downloaded, not committed.

`src/engine/eval/nnue/` is in the build and `evaluate` runs the forward pass
whenever a net is resident: the arenas, the accumulator bracket in the search, the
load path, the `EvalFile` option and the search's per-colour optimism term are all
wired. What remains is the per-square trace table, the deletion of the classical
fallback, and the differential check against upstream. See
[03-engine-eval.md](03-engine-eval.md).

**Gate:** `eval` output matches upstream to the last unit on every bench position,
scalar and vector paths agreeing.

### M4 — Threads and NUMA

Lazy-SMP: the thread pool, per-worker state, shared histories, thread voting, and
NUMA replication of the network weights. Determinism is the hard part — the search
must remain reproducible at fixed depth with one thread.

The pool, the thread runtime, the NUMA model and the large-page allocator are in
`src/platform/` and **in `SOURCES` and `ENGINE_SOURCES`**, alongside the per-worker
layout in `src/engine/state/`; they are unit-tested and covered by `./build.sh
tsan`. **Nothing calls them**, so the search is still single-threaded and
`Threads` above 1 is accepted and ignored — `./build.sh tsan-search` measures
that directly, reporting 0 races because the process never leaves one thread.
See [04-multithreading.md](04-multithreading.md), which lists what the wiring
commit has to decide.

**Gate:** single-threaded signature unchanged; multi-threaded runs converge to the
same bestmove; a NUMA-replicated run matches a non-replicated one.

### M5 — Syzygy

WDL and DTZ probing, the table registry, the decompression path, root and
in-search probing, and the UCI options.

Done for 3-man. The prober sits under `src/platform/syzygy/` behind the
`src/platform/tablebase.h` facade and is **in `SOURCES` and `ENGINE_SOURCES`**;
`src/shell/syzygy_option.c` owns the four UCI options and binds the
`tb_source.h` / `option_source.h` seams. With no `SyzygyPath` set
`tablebase_max_cardinality` is 0, so `load_tb_config` clamps `cardinality` to 0,
Step 6 never enters and the root ranking never runs — which is why wiring it left
`./build.sh signature` unchanged.

**Gate:** `./build.sh tb` — discovery and the root probe's score and tbhits over
an 11-position 3-man battery, diffed against a golden derived from the oracle.
Still open: 5-man tables, so the cursed-win / blessed-loss branches of
`map_score_dtz` and `probe_dtz` are unexercised.

### M6 — Bit-exact

Everything wired together, the differential gate green at `UPSTREAM_BASE`, and the
upstream-sync workflow running so the port tracks a moving upstream.

**Gate:** `./build.sh upstream-parity` — mcfish's bench equals the pristine
upstream build's bench, at the pinned SHA.

## Where mcfish is today

Read the counts from the map, not from this section:

```sh
./build.sh port-status
```

**The binary and the tree have drifted apart, and that is the fact to hold.** A
large majority of the ported `.c` files are on disk and absent from
[`../build.sh`](../build.sh)'s `SOURCES` array, which has no wildcard and no
dependency scanner. A file outside that array is compiled by nothing: not in the
binary, not linked by `zone-check`, not reached by `./build.sh test`, and not
covered by `signature`, `perft` or `golden`.

**What the binary is today:** a single-threaded engine with magic slider attacks,
upstream's Zobrist tables and threat deltas, the decomposed search with its full
pruning set and aspiration window, a staged move picker, the full history block,
the time manager, upstream's cluster transposition table, and the NNUE evaluation
with its incremental accumulator — falling back to a classical placeholder when no
net is resident, and upstream's full UCI option table — advertised byte for byte,
with `Threads`, `NumaPolicy` and the four Syzygy options accepted and inert. No
tablebases and no threads.

**What is written and not in the binary:** the Syzygy prober, the thread pool and
NUMA runtime, the per-worker state zone, the board-zone module split, and the
decomposed shell — including `engine.c`, whose own registration of the option set
is now a dead duplicate of the live one. Each zone page names its own modules and
what its wiring commit owes.

Treat that as the largest open item in the port, not as a staging area. A module
nothing compiles is a module nothing defends: it rots against the files that do
move, and the wiring commit pays the difference. **Wire a module in the commit that
finishes it.**

The classical term in `src/engine/eval/evaluate.c` is **scaffolding to be
deleted**, not a feature. It is the fallback for a run with no `.nnue` file on
disk. Do not tune it, do not extend it, and do not let it acquire callers that NNUE
will not satisfy.

## Rules while porting

- **One module per commit**, with the zfish source named in the body. A commit
  that ports three modules cannot be bisected when the node count moves.
- **Put the module in `SOURCES` in the same commit that ports it**, and in
  `ENGINE_SOURCES` too when it belongs to `engine/` or `platform/`. A module the
  build does not name is one no gate can defend, and the tree already carries more
  of those than it should.
- **Re-derive the signature only for an intended behaviour change**, and say what
  moved it. On a red gate, re-deriving launders a bug into the anchor.
- **Do not "improve" on zfish or upstream while porting.** A cleaner formulation
  that changes a rounding boundary changes the node count. Port faithfully first;
  the port map is not the place to land ideas.
- **Integer semantics are the classic trap.** C++ and Zig and C differ on
  conversion, shift, and overflow behaviour at the edges. Upstream relies on
  wrapping in places. Where zfish carries a comment about integer semantics,
  carry it across.
