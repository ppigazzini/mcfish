# Tooling and CI

Every `./build.sh` step and what it actually gates, the two source arrays that
decide what is gated at all, the golden-diff harness and what its normalization
throws away, the two kinds of expected-value file, the anchor versus the finish
line, and the two CI lanes.

Audience: all developers. The workflow around these gates is in
[`../CONTRIBUTING.md`](../CONTRIBUTING.md); the port sequence they are gating
toward is in [PORTING.md](PORTING.md).

## The arrays decide what is gated

Before the step table: **every gate below runs over the linked binary, and two
arrays in [`../build.sh`](../build.sh) enumerate what that is.**

- `SOURCES` — the release and debug binaries.
- `ENGINE_SOURCES` — `engine/` plus `platform/clock.c`, `platform/tablebase.c`
  and `platform/syzygy/`; what `zone-check` links standalone and what
  [`../tests/test_main.c`](../tests/test_main.c) is built against. The Syzygy
  files are here as well as in `SOURCES` because the engine zone must still link
  without `shell/`, and the prober is a platform service the engine reaches
  through `tb_source.h`.

There is no wildcard and no dependency scanner. A `.c` file in neither array is
compiled by nothing, so `build`, `test`, `zone-check`, `signature`, `perft` and
`golden` all pass over it without reading a line. **A green `parity` is a statement
about the arrays, not about `src/`.**

Most of the ported tree is in that state today — see
[00-architecture.md](00-architecture.md). Two consequences for anyone using these
gates:

- **Adding a file means editing `SOURCES`**, and `ENGINE_SOURCES` too if it belongs
  to `engine/` or `platform/`, or `zone-check` and the test binary will not see it.
- **An unwired file that stopped compiling three commits ago still shows green.**
  The first thing a wiring commit discovers is how far the tree moved underneath
  it. That is the cost of leaving a finished module out of the array, and it is why
  the port rule is one module per commit, wired.

## The steps

[`../build.sh`](../build.sh) is the whole build system and the whole in-repo gate
battery. `./build.sh help` prints the list; this table says what each step
*proves*.

| Step | What it does | What it gates |
| --- | --- | --- |
| `build` | clang `-O3 -DNDEBUG`, one invocation over `SOURCES` | that the files **in `SOURCES`** compile under the full warning set. Not the tree — see above |
| `debug` | the same sources with ASan + UBSan and `-fno-sanitize-recover=undefined` | nothing on its own; it is the binary the sanitizer lane drives |
| `zone-check` | links `ENGINE_SOURCES` plus a stub `main`, with no shell object | that no **listed** `engine/` file calls into `shell/`. It **links**, so a forbidden call is an undefined symbol rather than a clean compile. It cannot see the engine→platform edge, because `clock.c` is inside the array |
| `test` | builds `ENGINE_SOURCES` + [`../tests/test_main.c`](../tests/test_main.c) under ASan+UBSan and runs it | the unit and property suite: perft to reference counts, make/unmake round-trip, incremental-vs-recomputed Zobrist, search determinism |
| `tsan` | rebuilds `ENGINE_SOURCES` + the test binary under ThreadSanitizer and runs it | the thread pool: that spawning, dispatching a job, waiting on the condition variable and joining carry the happens-before edges they claim. **This is the only gate that can see a threading bug at all** — the single-threaded search never reaches that code, and a race does not have to fire to be there. Kept out of `parity`: it needs its own build of the engine and roughly triples the suite. Run it whenever `src/platform/thread*.c` changes |
| `tsan-search [depth] [threads]` | builds the **whole engine** under ThreadSanitizer and drives one `go` through the UCI front end | races in the SEARCH, which `tsan` cannot see: that step links the test binary, so the only concurrent code it reaches is the thread-pool test. Reports **0** today, and that is a measurement of a single-threaded process rather than a clean bill of health — see [04-multithreading.md](04-multithreading.md). It exists now so the first run after the pool is driven compares against a known-zero baseline |
| `signature` | runs `bench 8`, compares the node total to [`../tools/signature.golden`](../tools/signature.golden) | that no edit changed search behaviour unintentionally |
| `perft` | drives every row of [`../tools/perft.table`](../tools/perft.table) through the UCI front end | move generation totality |
| `golden` | diffs each `tools/cases/*.uci` transcript against its `.golden` | the observable UCI surface, byte for byte after normalization |
| `tb-fetch` | downloads the 3-man Syzygy set (KPvK KNvK KBvK KRvK KQvK, WDL+DTZ) into `resources/syzygy/` | nothing — it *fetches*. It verifies each file's Syzygy magic (`.rtbw` `71 E8 23 5D`, `.rtbz` `D7 66 0C A5`) and deletes anything that fails, so a mirror's HTML error page cannot masquerade as a table |
| `tb` | runs the discovery report and the root probe battery in [`../tools/cases/tb.fens`](../tools/cases/tb.fens), diffed against [`../tools/tb.golden`](../tools/tb.golden) | Syzygy discovery, the root DTZ/WDL ranking and the probe path. **Without the tables it checks discovery only and says so in red** — the probe half reads as unexercised, never as a pass |
| `fmt` / `fmt-fix` | `clang-format --dry-run --Werror` over `src/` and `tests/` | formatting. Exits **127** when no `clang-format` is found |
| `docs-lint` | [`../tools/docs_lint.sh`](../tools/docs_lint.sh) | dead internal links, named paths that do not exist, a quoted bench signature. See [11-writing.md](11-writing.md) |
| `port-status` | [`../tools/port_status.sh`](../tools/port_status.sh) over the port map | nothing — it *reports*. It is the number to quote instead of writing one down |
| `upstream-parity` | [`../tools/upstream/upstream_parity.sh`](../tools/upstream/upstream_parity.sh) | the finish line: mcfish's bench against a pristine upstream build. Red until the port completes — see below |
| `parity` | the aggregate | the nine gates listed below it — every in-repo gate, and neither `upstream-parity` nor `port-status` |
| `net` | names the `.nnue` this build expects, lists the directories the engine searches, prints the download command, and says whether the file is present | nothing — it *reports*. It never downloads: the net is a runtime input, not a build product, and fetching it would make every clean build a network dependency |
| `bench` / `clean` | run the benchmark; remove `build/` | nothing |
| `signature-update` / `golden-update` | re-derive an anchor | read the warning below before running either |

`parity` runs: `build`, `zone-check`, `fmt`, `docs-lint`, `test`, `signature`,
`perft`, `golden`, `tb`.

`tools/tb.golden` is **oracle-derived**: `./build.sh tb-update` regenerates it by
running the pristine upstream binary over the same battery, never mcfish. It pins
each position's root `score` and `tbhits` from the **depth-1** info line and
deliberately pins neither nodes, pv nor bestmove — upstream early-returns at depth
1 once the root is in the tablebase while mcfish searches on, and among
equally-optimal TB moves either may pick a different winning one. Gating that
would be fake parity. See [`../tools/GOLDEN_PROVENANCE.md`](../tools/GOLDEN_PROVENANCE.md).

### A skipped gate is not a passing gate

`fmt` exits 127 when `clang-format` is absent. `parity` treats that as *skipped*,
keeps going, and then **names every skipped gate in its summary line** — because
"parity passed" printed over a silently absent linter is exactly how a gate rots
into decoration. It is the only gate here that can be skipped; every other one
runs on a bare toolchain.

## Regenerating a golden on a red gate launders a bug

This is the most expensive mistake available in this repository, so it gets its
own section.

`signature-update` and `golden-update` do not verify anything. They run the
binary and write down whatever it produced. If the gate was red because the code
is wrong, the update **writes the defect into the anchor**, the gate goes green,
and every future run asserts the bug.

`golden-update` prints a warning to that effect and `signature.golden` carries it
in a comment header. Neither can stop you.

The rule, with the case the obvious version of it forbids:

- **Green gate, intended behaviour change** → re-derive, and say in the commit
  body what moved it. This is the normal case: a ported module changes the node
  count by design.
- **Red gate** → fix the code. Do not re-derive.
- **Red gate that is a *fidelity fix*** — the code was wrong, you corrected it
  toward upstream, and the anchor is now stale — → re-derive, but the commit body
  must state the defect, the correction, and the evidence that the new value is
  the right one. That evidence is upstream, not the binary you just ran.

The distinction between the last two is not mechanical and no gate can make it.
It is the reason the commit body is part of the gate.

## Fact tables versus goldens

Two kinds of expected-value file live in `tools/`, and confusing them is how a
real bug gets normalised away.

**[`../tools/perft.table`](../tools/perft.table) is not a golden.** Its counts are
mathematical facts about chess — the number of leaves in the legal tree below a
position. They do not depend on this engine, on its evaluation, on its search, or
on the port's progress. **A perft mismatch is always a move generation bug**, and
there is no circumstance in which the correct response is to edit the number. The
same is true of the deep counts hardcoded in
[`../.github/workflows/mcfish_perft.yml`](../.github/workflows/mcfish_perft.yml).

**`tools/*.golden` are goldens.** They record what *this* binary printed, and they
move legitimately whenever behaviour changes on purpose. `tools/signature.golden`
is a golden too — see the next section for what it is and is not.

## The anchor and the finish line

Two node counts. They are not the same number and must never be conflated.

| | What it is | Where |
| --- | --- | --- |
| **The anchor** | mcfish's *current* bench total. Exists so a refactor cannot silently change behaviour today. | [`../tools/signature.golden`](../tools/signature.golden), asserted by `./build.sh signature` |
| **The finish line** | upstream Stockfish's own `Bench:` for the pinned commit. The target of the whole port. | derived from the SHA in [`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE) |

The anchor is expected to move repeatedly as modules land. The finish line does
not move until the pin does. `./build.sh port-status` prints the pinned base
alongside the module counts.

**No number for either appears anywhere in this documentation set**, and
`docs-lint` fails a page that quotes the anchor. Read it from the file, or run the
gate.

`./build.sh upstream-parity` is the M6 gate: it builds a **pristine** upstream
Stockfish at the pinned SHA into a detached worktree outside the repo, runs both
benches, and compares the totals.

Pristine is the point. The oracle is upstream's own source built by upstream's own
Makefile, with no mcfish edit near it — a shared tree would let a bug present in
both cancel out and pass.

**It is red today, and that is correct**, not a regression: mcfish cannot match
upstream's node count while the evaluation and search are unported. That is
exactly why it is **not** part of `./build.sh parity`, which must stay green on a
correct in-progress tree. Run it deliberately, and read the milestone it gates in
[PORTING.md](PORTING.md).

## The golden-diff harness

`do_golden` runs each script in `tools/cases/` through the binary, merges stdout
and stderr, pipes the result through `normalize()`, and diffs against
`tools/<name>.golden`.

The cases cover the board dump, malformed input, the eval trace, the UCI
handshake, perft output, and a search transcript.

### normalize(), and what it costs

```bash
sed -E 's/ nps [0-9]+//; s/ time [0-9]+//;
        s/^Total time \(ms\) *: [0-9]+$/Total time (ms) : <elided>/;
        s/^Nodes\/second *: [0-9]+$/Nodes\/second    : <elided>/'
```

Four fields, all wall-clock derived, all elided. A golden must pin **behaviour**,
not the speed of the machine that produced it; without this, every golden fails on
a runner faster or slower than the developer's.

**`nodes` is deliberately not normalized.** The node count is a deterministic
function of the search, so it is exactly the field a golden should hold — eliding
it would leave the search transcripts asserting little more than that the engine
printed some lines.

Keep the list minimal, and read it as a list of things no golden guards. Every
field added here is a field that can drift forever without a gate noticing.

## Local-only measurement tooling

Four scripts in `tools/` that are **not** `./build.sh` steps and **not** gates.
They measure the host they run on, and a shared, thermally-uncontrolled CI runner
cannot carry a performance verdict — so they are deliberately kept out of
`parity` and out of the workflows.

| Tool | Answers |
| --- | --- |
| [`../tools/nps_ab.sh`](../tools/nps_ab.sh) | the headline speed ratio, interleaved and paired |
| [`../tools/perf_callgrind.sh`](../tools/perf_callgrind.sh) | deterministic instructions, D refs and cache misses |
| [`../tools/perf_fingerprint.py`](../tools/perf_fingerprint.py) | per-function attribution, and the call-count parity test |
| [`../tools/valgrind.sh`](../tools/valgrind.sh) | memcheck: invalid access, bad free, definite leak |

**Pick by size of the effect.** `nps` cannot resolve anything under about 5% —
wall-clock on this class of hardware swings by more than that between batches, so
it has both falsely confirmed and falsely refuted real changes. callgrind is
deterministic and resolves 0.01%. Use `nps_ab.sh` for the headline, callgrind for
anything smaller.

Four rules that each cost a wrong number before they were written down:

- **Same tree or nothing.** Both engines must report the identical node count;
  a different count is a different workload and the ratio is void. `nps_ab.sh`
  asserts this and refuses to run.
- **Same ARCH.** Build every side at `x86-64-sse41-popcnt` — mcfish's default
  `MCFISH_ARCH=sse41`, which matches the oracle's. A native build against an
  SSE4.1 one measures the ISA tier, not the code. callgrind also SIGILLs above
  that tier.
- **Same compiler backend, for any cost ratio.** The bench-parity oracle is
  built with gcc, and node counts are compiler-independent so that is fine for
  `upstream-parity`. It is *not* fine for an instruction ratio: measuring against
  it compares gcc with LLVM. Build a separate reference with `zig c++` (or clang)
  for perf work.
- **Subtract startup.** On a shallow bench the net load, magic init and zero-fill
  are ~37% of the profile, and they are *cheaper* in mcfish than upstream — so
  the whole-process ratio reads 0.987x where the search-only ratio is 1.19x.
  Profile `printf 'quit\n' | <bin>` for a startup figure and subtract it, or name
  the offenders with `perf_fingerprint.py costs`.

**Call counts, not costs, are the parity test.** `perf_fingerprint.py --calls`
answers "do we run Stockfish's algorithm?" — call counts are inlining-immune,
costs are not. Group on the symbols that exist in *your* build: clang inlines
upstream's affine layers into `Network::evaluate` while mcfish keeps
`nnue_affine_32` as a symbol, and upstream has two `do_move` overloads. A regex
written against the wrong side reads a divergence that is not there.

## CI

Two workflows in [`../.github/workflows/`](../.github/workflows). None of them
does anything a developer cannot reproduce with `./build.sh`; anything that
diverges is a bug in the workflow file.

### `mcfish_parity.yml` — the blocking lane

Runs on every push and PR, with four jobs:

- **`fmt`** — split out and run first because it is the cheapest signal. It
  duplicates the `fmt` inside `parity` on purpose: whitespace drift caught in a
  minute beats whitespace drift caught fifteen minutes in. clang-format is
  installed at the **same pinned major** as the compiler, because a different
  major reflows code that was clean under another one and the gate would flap.
- **`parity`** — `./build.sh parity` and nothing else, after asserting the
  installed clang is new enough for the C23 the tree uses. The pin is explicit so
  a toolchain regression is attributable to a commit rather than to a floating
  runner image.
- **`sanitizers`** — ASan+UBSan over paths `parity`'s test binary never reaches:
  the release search at bench depth and the perft path through `shell/`. Kept
  separate because the instrumented binary is roughly an order of magnitude
  slower.
- **`gcc`** — the second-compiler lane, and the subtlest gate here. It builds with
  gcc under the same `-std` and warning set, then holds the **gcc-built binary to
  the clang-derived anchor**. Two conforming compilers must produce the same node
  count from the same deterministic integer search. A disagreement means
  undefined behaviour the optimisers exploited differently, or reliance on
  implementation-defined behaviour. **It is never "expected compiler variation"
  and must never be resolved by re-deriving the golden.**

  This lane is the reason the `packed struct` and wrapping-arithmetic rules in
  [08-idiomatic-c.md](08-idiomatic-c.md) are rules and not preferences.

### `mcfish_perft.yml` — nightly deep perft

The push lane's perft is capped by a sub-minute budget, and depth is what perft
coverage is made of: en passant exposing a pin, a castling right lost to a rook
capture several plies back, an under-promotion with check. Those live a few plies
below where the fast gate stops. This lane spends the time, against published
reference counts, on the six standard positions. Same rule as above — the counts
are facts, so a mismatch is a movegen bug.
