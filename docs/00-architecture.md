# Architecture

How the code is structured: the three zones, how they depend on each other, how
that dependency is enforced in C, which files are actually in the binary, and how
one search flows through them. For the build and the gate battery see
[09-tooling-ci.md](09-tooling-ci.md); for the C patterns behind the hot path see
[08-idiomatic-c.md](08-idiomatic-c.md). Per-file detail lives in each header's
leading comment block.

This page states structure, not numbers. Where a figure would date the page, name
the command that computes it.

**The zone layout is the engine's shape.** Each zone is decomposed into small
single-responsibility modules. The authoritative list of what is compiled is
`build.sh`'s `SOURCES` array; a file outside it is in the tree but not the binary.

## The three zones

`src/` splits by responsibility, one directory each:

| Zone | Path | Owns | Allowed to include |
| --- | --- | --- | --- |
| **engine** | `src/engine/` | the chess library: types, bitboards, position, movegen, search, per-worker state, TT, evaluation | nothing outside `engine/` |
| **platform** | `src/platform/` | the OS runtime: the clock, memory, threads, NUMA, the Syzygy prober | `engine/` |
| **shell** | `src/shell/` | the process: `main`, the UCI loop, the option table, bench | `engine/`, `platform/` |

The intended stack is `shell -> platform -> engine`, engine at the bottom.
`platform/` is not a layer *beneath* the engine: it is the runtime that hosts the
engine, so it is allowed to depend on engine types, not the other way round.

```mermaid
flowchart TD
    shell["src/shell/ — the process"]
    platform["src/platform/ — the OS runtime"]
    engine["src/engine/ — the chess library"]

    shell --> engine
    shell --> platform
    platform --> engine
    engine -.->|"search.c includes platform/clock.h"| platform

    style engine fill:#1f6f3f,color:#fff
```

**The dashed edge is real and is a gap, not a design — but it has shrunk to one
file.** The search zone itself reads the clock through the injection seam
[`src/engine/search/time_source.h`](../src/engine/search/time_source.h), the same
shape `output_sink.h` uses for output; no node body, no time decision and no info
line calls into `platform/`. What remains is the facade
[`src/engine/search/search.c`](../src/engine/search/search.c), which includes
`../../platform/clock.h` to *register* `now_ms` as that seam's implementation and to
stamp the search's start time.

That registration belongs in the shell, which is the zone that already owns
`search_set_output`. It sits in the facade only because the live shell has no
startup hook that runs before the first `go`. The cost is unchanged in kind and
smaller in degree: `engine/` still cannot be linked without a POSIX clock, so the
zone is not yet the standalone library the header comments claim.

## What is actually in the binary

**There is no dependency scanner and no wildcard.** Two arrays in
[`../build.sh`](../build.sh) enumerate every translation unit:

- `SOURCES` — what the release and debug binaries are built from.
- `ENGINE_SOURCES` — `engine/` plus all of `platform/` (the clock, memory, the
  thread runtime and pool, NUMA, `tablebase.c` and `syzygy/`), what `zone-check`
  links standalone and what [`../tests/test_main.c`](../tests/test_main.c) is built
  against.

A `.c` file that is in neither array is compiled by nothing. It is not in the
binary, not linked by `zone-check`, not reached by `./build.sh test`, and not
covered by `signature`, `perft` or `golden`. **Read the arrays, not the
directory listing, to know what the engine is.**

The decomposed shell is the tree's one remaining subsystem in that state today.
The zone pages name the specific modules — see
[01-engine-board.md](01-engine-board.md),
[02-engine-search.md](02-engine-search.md),
[03-engine-eval.md](03-engine-eval.md), [06-platform.md](06-platform.md) and
[07-shell.md](07-shell.md). The shape of the gap is always the same: a module was
ported and checked in isolation, and until it enters `SOURCES` nothing re-checks
it, so it rots silently against the files that do move.

Adding a file therefore means editing `SOURCES`, and — if it belongs to
`engine/` or `platform/` — `ENGINE_SOURCES` as well, or `zone-check` and the test
binary will not see it.

## How the zone rule is enforced

C has no module system, so there is no import graph to lint and no compiler error
for a stray include. The enforcement here is **link-time**, and it lives in
[`../build.sh`](../build.sh):

```bash
./build.sh zone-check
```

`do_zone_check` compiles the `ENGINE_SOURCES` list together with a generated stub
`main` and links the result. No `src/shell/` object is on the command line. If an
engine file has grown a call into `uci.c` or `benchmark.c`, the link fails with an
undefined symbol.

Three properties of this check matter:

- **It links, it does not merely compile.** Compiling would only prove the
  declarations are visible; linking is what proves no call is left dangling. A
  forbidden call to a shell function compiles fine against any prototype and fails
  only at link time.
- **It cannot see the engine→platform edge.** `clock.c` is *inside*
  `ENGINE_SOURCES`, so `search.c`'s include of `platform/clock.h` resolves and the
  check passes. `zone-check` proves exactly one thing: engine plus platform links
  without shell. It says nothing about the boundary between engine and platform.
- **It cannot see a file that is not in the array.** An unwired engine module
  could call straight into `shell/` and `zone-check` would stay green, because it
  never compiles that file at all.

[`../tests/test_main.c`](../tests/test_main.c) is built from the same
`ENGINE_SOURCES` list, so `./build.sh test` is a second, independent instance of the
same check with the same blind spots: a test that needs a shell symbol does not
link.

## The composition root

[`src/shell/main.c`](../src/shell/main.c) is the composition root. It is the only
file that may include across every zone, and nothing includes it.

```c
int main(int argc, char **argv) {
    bitboards_init();
    attacks_init();
    threats_init();  // build RayPassBB, which reads the attack tables
    position_init();
    eval_nnue_init();

    uci_loop(argc, argv);
    search_shutdown();
    eval_nnue_shutdown();
    return 0;
}
```

**The order is load-bearing, and its failure mode is silent.**

1. `bitboards_init()` fills `SquareBB` in
   [`src/engine/board/bitboard.c`](../src/engine/board/bitboard.c). That header is
   the std-only leaf; it holds no attack tables.
2. `attacks_init()` runs the magic search in
   [`src/engine/board/attacks.c`](../src/engine/board/attacks.c) and derives
   `PseudoAttacks`, `PawnAttacksBB`, `BetweenBB` and `LineBB` from it.
3. `threats_init()` builds `RayPassBB` in
   [`src/engine/board/threats.c`](../src/engine/board/threats.c), which reads the
   attack tables step 2 just filled.
4. `position_init()` fills the Zobrist tables in
   [`src/engine/board/position.c`](../src/engine/board/position.c) from a
   fixed-seed generator.
5. `eval_nnue_init()` builds the NNUE feature index tables and allocates the two
   accumulator arenas. Those tables are **zero, not garbage**, beforehand, so a
   missing call is a silent all-zero feature set rather than a crash — the same
   failure shape as step 2, one zone over. The net itself is *not* loaded here: it
   is a runtime input the UCI layer loads, because the UCI layer owns the
   `EvalFile` option. See [03-engine-eval.md](03-engine-eval.md).
6. Only then may any `Position` exist. `pos_set` calls `set_check_info`, which
   reads `PseudoAttacks` and `BetweenBB`.

`main` pairs the init with `search_shutdown()` and `eval_nnue_shutdown()` after
`uci_loop` returns.

A `Position` built before step 2 does not crash. It reads zeroed attack sets, so
`slider_blockers` finds no snipers, `set_check_info` finds no checkers, and
`generate` emits no piece moves. The engine comes up, answers `uci`, accepts
`position`, and searches a board on which nothing attacks anything — a failure that
presents as a search bug, not a startup bug. That is why the order is stated in
`main.c`'s header comment and repeated here: the check that catches it is a human
reading the call sequence.

**`repetition_init` runs inside `position_init`, not beside it.**
[`src/engine/board/repetition.c`](../src/engine/board/repetition.c) builds the
cuckoo table of reversible one-piece move keys, which `pos_upcoming_repetition`
probes at the top of every non-root node. The table is a pure function of the
Zobrist psq and side keys, so it is built where those keys are drawn and takes them
as arguments rather than re-deriving them — a second independently seeded PRNG copy
is exactly the drift it cannot survive. Leaving it uninitialised does not fail
anything: an all-zero table turns a cycle-detection cutoff into a silent no-op,
which costs nodes and moves the signature without raising an error.

The same init sequence opens [`../tests/test_main.c`](../tests/test_main.c)'s
`main`, in the same order, for the same reason.

## The output seam

The engine zone never calls `printf`. `search_go` and `perft` emit their `info` and
perft-divide lines through a function pointer installed by the shell:

```c
// src/engine/search/search.h — the leaf declares the seam.
void search_set_output(void (*emit)(const char *line));

// src/shell/uci.c — the composition root injects the real sink at startup.
search_set_output(emit_stdout);
```

`Emit` starts as `nullptr` and `emit_line` checks it, so an unregistered sink is
silence, not a crash. That is the correct default for a library — but note the
consequence: a gate that forgets to install a sink sees a search that runs and
prints nothing, which looks like a hung engine rather than a wiring bug.

This seam is why `engine/` links without `shell/`, and why a future in-process gate
can capture search output without spawning a subprocess. The clock is the one
service that did **not** get this treatment in the live search; see the dashed edge
above.

## How a search flows

```mermaid
flowchart TD
    M["src/shell/main.c<br/>bitboards → attacks → threats → position"]
    U["src/shell/uci.c<br/>uci_loop: parse, options, TT sizing"]
    B["src/shell/benchmark.c<br/>fixed FEN set, TT cleared each"]
    SG["engine/search/search.c + search_id.c<br/>search_go → iterative_deepening"]
    AB["search_main.c + search_back.c<br/>search_node → search_run_back"]
    QS["engine/search/search_qsearch.c<br/>qsearch_node: captures + stand-pat"]
    MP["engine/search/movepick.c<br/>staged picker + see_ge"]
    HI["engine/search/history.c<br/>the history block"]
    TM["engine/search/timeman.c<br/>the budget"]
    MG["engine/board/movegen.c<br/>generate / generate_legal"]
    PO["engine/board/position.c<br/>pos_do_move / pos_undo_move"]
    TH["engine/board/threats.c<br/>the threat delta"]
    TT["engine/search/tt.c<br/>probe / save"]
    EV["engine/eval/evaluate.c<br/>NNUE, or the classical fallback"]
    NN["engine/eval/nnue/<br/>accumulator + forward pass"]
    CL["platform/clock.c<br/>now_ms"]

    M --> U
    U -->|"go"| SG
    U -->|"bench"| B --> SG
    SG --> TM
    SG --> AB
    AB -->|"depth <= 0"| QS
    AB -->|"recurse"| AB
    AB --> MP
    AB --> HI
    AB --> PO --> TH
    AB --> TT
    AB --> EV
    QS --> EV
    QS --> MP
    MP --> MG
    EV --> NN
    PO -.->|"writes the delta into the accumulator arena"| NN
    SG -.->|"deadline"| CL
    AB -.->|"every 512 nodes"| CL
```

`uci_loop` parses a `go` line into a `SearchLimits` and calls `search_go`.
`search_go` resolves the time budget once through `timeman_init`, seeds a legal
move, then deepens: each iteration runs `alphabeta` from the root, reads the best
move back out of the TT, and emits one `info` line through the sink. `alphabeta`
recurses, dropping into `qsearch` at depth zero; both pull moves from the staged
`MovePicker`, apply them with `pos_do_move`/`pos_undo_move`, and score leaves with
`evaluate`.

Each real make/unmake in that recursion is bracketed by `eval_acc_push` /
`eval_acc_pop`, so `pos_do_move` writes its NNUE delta straight into the
accumulator's own arena rather than into a copy. That bracket is a contract, not an
optimisation: skip it and the accumulator silently describes a different position
than the board does. The null move and `perft` are the two deliberate exceptions —
see [03-engine-eval.md](03-engine-eval.md) for why an empty push is not the same as
no push.

The only wall-clock read inside the recursion is the one guarded by the
node-count checkpoint in `check_time` — see
[02-engine-search.md](02-engine-search.md) for why that guard is what keeps the
signature gate meaningful.

Nothing on that path allocates. The move lists are `ExtMove list[MAX_MOVES]`
automatics and the `StateInfo` per ply is an automatic in the recursing frame; the
engine's three heap blocks — the transposition table, the NNUE accumulator stack and
the refresh cache — are all allocated once, outside any search.

## What is on disk but not in the binary

The `SOURCES` array is the sole authority on what is compiled. One decomposition
sits in the tree outside it:

**The decomposed shell** — `src/shell/engine.c`, `uci_parse.c` and their siblings —
is on disk but not in `SOURCES`, so not in the binary and not gated. `uci.c` remains
the live monolith and holds the session state (the position, the state chain and the
option table) as file-scope statics; `engine.c` registers a second, dead copy of the
option set. Treat those files as unwired scratch, not as the live shell.

The zone diagram above is the shape all of that lands into.
