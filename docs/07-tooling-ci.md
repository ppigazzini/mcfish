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
- `ENGINE_SOURCES` — `engine/` plus `platform/clock.c`; what `zone-check` links
  standalone and what [`../tests/test_main.c`](../tests/test_main.c) is built
  against.

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
| `signature` | runs `bench 8`, compares the node total to [`../tools/signature.golden`](../tools/signature.golden) | that no edit changed search behaviour unintentionally |
| `perft` | drives every row of [`../tools/perft.table`](../tools/perft.table) through the UCI front end | move generation totality |
| `golden` | diffs each `tools/cases/*.uci` transcript against its `.golden` | the observable UCI surface, byte for byte after normalization |
| `fmt` / `fmt-fix` | `clang-format --dry-run --Werror` over `src/` and `tests/` | formatting. Exits **127** when no `clang-format` is found |
| `docs-lint` | [`../tools/docs_lint.sh`](../tools/docs_lint.sh) | dead internal links, named paths that do not exist, a quoted bench signature. See [09-writing.md](09-writing.md) |
| `port-status` | [`../tools/port_status.sh`](../tools/port_status.sh) over the port map | nothing — it *reports*. It is the number to quote instead of writing one down |
| `upstream-parity` | [`../tools/upstream/upstream_parity.sh`](../tools/upstream/upstream_parity.sh) | the finish line: ccfish's bench against a pristine upstream build. Red until the port completes — see below |
| `parity` | the aggregate | the eight gates listed below it — every in-repo gate, and neither `upstream-parity` nor `port-status` |
| `net` | names the `.nnue` this build expects, lists the directories the engine searches, prints the download command, and says whether the file is present | nothing — it *reports*. It never downloads: the net is a runtime input, not a build product, and fetching it would make every clean build a network dependency |
| `bench` / `clean` | run the benchmark; remove `build/` | nothing |
| `signature-update` / `golden-update` | re-derive an anchor | read the warning below before running either |

`parity` runs: `build`, `zone-check`, `fmt`, `docs-lint`, `test`, `signature`,
`perft`, `golden`.

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
[`../.github/workflows/ccfish_perft.yml`](../.github/workflows/ccfish_perft.yml).

**`tools/*.golden` are goldens.** They record what *this* binary printed, and they
move legitimately whenever behaviour changes on purpose. `tools/signature.golden`
is a golden too — see the next section for what it is and is not.

## The anchor and the finish line

Two node counts. They are not the same number and must never be conflated.

| | What it is | Where |
| --- | --- | --- |
| **The anchor** | ccfish's *current* bench total. Exists so a refactor cannot silently change behaviour today. | [`../tools/signature.golden`](../tools/signature.golden), asserted by `./build.sh signature` |
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
Makefile, with no ccfish edit near it — a shared tree would let a bug present in
both cancel out and pass.

**It is red today, and that is correct**, not a regression: ccfish cannot match
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

## CI

Two workflows in [`../.github/workflows/`](../.github/workflows). None of them
does anything a developer cannot reproduce with `./build.sh`; anything that
diverges is a bug in the workflow file.

### `ccfish_parity.yml` — the blocking lane

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
  [06-idiomatic-c.md](06-idiomatic-c.md) are rules and not preferences.

### `ccfish_perft.yml` — nightly deep perft

The push lane's perft is capped by a sub-minute budget, and depth is what perft
coverage is made of: en passant exposing a pin, a castling right lost to a rook
capture several plies back, an under-promotion with check. Those live a few plies
below where the fast gate stops. This lane spends the time, against published
reference counts, on the six standard positions. Same rule as above — the counts
are facts, so a mismatch is a movegen bug.
