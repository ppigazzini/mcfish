# Tooling and CI

Every `./build.sh` step and what it actually gates, the two source arrays that
decide what is gated at all, the golden-diff harness and what its normalization
throws away, the two kinds of expected-value file, the anchor versus the finish
line, and the two CI lanes.

Audience: all developers. The workflow around these gates is in
[`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## The arrays decide what is gated

Before the step table: **every gate below runs over the linked binary, and two
arrays in [`../build.sh`](../build.sh) enumerate what that is.**

- `SOURCES` — the release and debug binaries.
- `ENGINE_SOURCES` — `engine/` plus all of `platform/` (the clock, memory, the
  thread runtime and pool, NUMA, `tablebase.c` and `syzygy/`); what `zone-check`
  links standalone and what
  [`../tests/test_main.c`](../tests/test_main.c) is built against. The Syzygy
  files are here as well as in `SOURCES` because the engine zone must still link
  without `shell/`, and the prober is a platform service the engine reaches
  through `tb_source.h`.

There is no wildcard and no dependency scanner. A `.c` file in neither array is
compiled by nothing, so `build`, `test`, `zone-check`, `signature`, `perft` and
`golden` all pass over it without reading a line. **A green `parity` is a statement
about the arrays, not about `src/`.**

No file is in that state today — see [00-architecture.md](00-architecture.md) for
the check that establishes it. The arrays covering the tree is the condition that
makes a green `parity` mean what a reader expects, so it is worth re-checking
whenever a file is added. Two consequences for anyone using these gates:

- **Adding a file means editing `SOURCES`**, and `ENGINE_SOURCES` too if it belongs
  to `engine/` or `platform/`, or `zone-check` and the test binary will not see it.
- **An unwired file that stopped compiling three commits ago still shows green.**
  The first thing a wiring commit discovers is how far the tree moved underneath
  it. That is the cost of leaving a finished module out of the array, and it is why
  the port rule is one module per commit, wired.

## Why a shell script and not a Makefile

The question is worth re-deriving rather than assuming, so here are the numbers on
this tree:

| | wall | CPU |
| --- | ---: | ---: |
| one `clang -flto` invocation over all 69 sources | **6.7 s** | 5.3 s |
| a comparable codebase built per-TU with `make -j8` | 16.2 s | 64.7 s |

A single invocation is **2.4x faster in wall clock and 12x cheaper in CPU** than
parallel per-TU compilation, because it parses each header once instead of once per
translation unit and does one LTO pass instead of compile-then-link. Make's headline
advantage — incremental, parallel builds — is *negative* here, and incremental buys
little against a 7-second full build that must relink the whole program under LTO
anyway.

The rest of this file is 36 workflow steps: golden diffs, tablebase fetches, a
three-phase PGO build, per-tier determinism sweeps. Make is a poor workflow runner
(the recipes are shell regardless), so a Makefile would wrap this script rather than
replace it.

**The one thing make gives free was worth taking, though**, and it is done better
than make does it. `need_binary` used to rebuild only when the binary was ABSENT, so
a gate run after an edit asserted against the previous binary and could report green
over code it had never compiled.

It now rebuilds when the binary does not match a **content hash** of every source,
every header, the full compile command and the compiler's own version, stored beside
the binary as `$BIN.stamp`. Timestamps — make's model — are wrong in both directions
here: they rebuild for nothing on a `git checkout`, a `touch`, or a save that changed
no bytes, and they MISS the case that matters most, because `MCFISH_ARCH` changes no
file. Under timestamps, building at sse41 and then running a gate with
`MCFISH_ARCH=native` left the sse41 binary in place and gated it while reporting the
native tier — the same trap `perf-budget` documents, and one that silently voids any
per-tier comparison. Hashing the flags closes it.

## The steps

[`../build.sh`](../build.sh) is the whole build system and the whole in-repo gate
battery. `./build.sh help` prints the list; this table says what each step
*proves*.

| Step | What it does | What it gates |
| --- | --- | --- |
| `build` | clang `-O3 -DNDEBUG`, one invocation over `SOURCES` | that the files **in `SOURCES`** compile under the full warning set. Not the tree — see above |
| `debug` | the same sources with ASan + UBSan and `-fno-sanitize-recover=undefined` | nothing on its own; it is the binary the sanitizer lane drives |
| `zone-check` | links `ENGINE_SOURCES` plus a stub `main`, with no shell object | that no **listed** `engine/` file calls into `shell/`. It **links**, so a forbidden call is an undefined symbol rather than a clean compile. It cannot see the engine→platform edge — all of `platform/` is inside the array — which is what `engine-standalone` is for |
| `engine-standalone` | compiles every `src/engine/*.c` alone and links them with **no** platform object | the engine→platform edge, as a count rather than a claim. Ratchets the undefined symbols against [`../tools/engine_platform.baseline`](../tools/engine_platform.baseline): a new one fails (fix it with a seam, not a baseline edit), and a stale one fails too, so the list cannot outlive what it measures. See [00-architecture.md](00-architecture.md) |
| `test` | builds `ENGINE_SOURCES` + [`../tests/test_main.c`](../tests/test_main.c) under ASan+UBSan and runs it | the unit and property suite: perft to reference counts, make/unmake round-trip, incremental-vs-recomputed Zobrist, search determinism |
| `tsan` | rebuilds `ENGINE_SOURCES` + the test binary under ThreadSanitizer and runs it | the thread pool: that spawning, dispatching a job, waiting on the condition variable and joining carry the happens-before edges they claim. **This is the only gate that can see a threading bug at all** — the single-threaded search never reaches that code, and a race does not have to fire to be there. Kept out of `parity`: it needs its own build of the engine and roughly triples the suite. Run it whenever `src/platform/thread*.c` changes |
| `tsan-search [depth] [threads]` | builds the **whole engine** under ThreadSanitizer and drives one `go` through the UCI front end | races in the SEARCH, which `tsan` cannot see: that step links the test binary, so the only concurrent code it reaches is the thread-pool test. Now that the pool is driven it measures a genuinely multi-threaded search — see [04-multithreading.md](04-multithreading.md). It is the search-race gate the thread-pool `tsan` run cannot substitute for |
| `signature` | runs `bench 8`, compares the node total to [`../tools/signature.golden`](../tools/signature.golden) | that no edit changed search behaviour unintentionally |
| `perft` | drives every row of [`../tools/perft.table`](../tools/perft.table) through the UCI front end | move generation totality |
| `golden` | diffs each `tools/cases/*.uci` transcript against its `.golden` | the observable UCI surface, byte for byte after normalization |
| `tb-fetch` | downloads the 3-man Syzygy set (KPvK KNvK KBvK KRvK KQvK, WDL+DTZ) into `resources/syzygy/` | nothing — it *fetches*. It verifies each file's Syzygy magic (`.rtbw` `71 E8 23 5D`, `.rtbz` `D7 66 0C A5`) and deletes anything that fails, so a mirror's HTML error page cannot masquerade as a table |
| `tb` | runs the discovery report and the root probe battery in [`../tools/cases/tb.fens`](../tools/cases/tb.fens), diffed against [`../tools/tb.golden`](../tools/tb.golden) | Syzygy discovery, the root DTZ/WDL ranking and the probe path. **Without the tables it checks discovery only and says so in red** — the probe half reads as unexercised, never as a pass |
| `fmt` / `fmt-fix` | `clang-format --dry-run --Werror` over `src/` and `tests/` | formatting. Exits **127** when no `clang-format` is found |
| `docs-lint` | [`../tools/docs_lint.sh`](../tools/docs_lint.sh) | dead internal links, named paths that do not exist, a quoted bench signature, a backticked `snake_case` symbol absent from the whole tree, and a `build.sh` step no tracked page mentions. See [11-writing.md](11-writing.md) |
| `upstream-parity` | [`../tools/upstream/upstream_parity.sh`](../tools/upstream/upstream_parity.sh) | mcfish's bench against a pristine upstream build — see below |
| `upstream-transcript` | drives both engines over [`../tools/cases/transcript/`](../tools/cases/transcript) and diffs the whole output | the UCI SURFACE against the golden, which `golden` structurally cannot do — that gate pins what mcfish printed last time, so both sides move together when a golden is re-derived. Machine-dependent fields are elided and nothing else; accepted divergences carry an argued reason in [`../tools/transcript_known.txt`](../tools/transcript_known.txt). **LOCAL** — it needs the oracle build |
| `parity` | the aggregate | the ten gates listed below it — every in-repo gate, but not `upstream-parity` |
| `net` | names the `.nnue` this build expects, lists the directories the engine searches, prints the download command, and says whether the file is present | nothing — it *reports*, and deliberately does not fetch, so that `build` never becomes a network dependency |
| `net-fetch` | downloads the expected net into `resources/` and **sha256-verifies** it | nothing — it fetches. It is a separate step precisely so `net` can stay offline; this is what the CI lanes run before a gate that needs a net |
| `simd-scalar` | rebuilds with `MCFISH_SIMD_SCALAR` — every vector type and intrinsic compiled out — and re-asserts the anchor | that `simd.h`'s two implementations are value-identical. In `parity`, and the only gate that can see a portable-spelling/scalar divergence |
| `arch-determinism` | builds every ISA tier the host can execute and requires one node count | that the evaluation is arch-invariant — **and, since the tiers now run different ALGORITHMS, that those algorithms agree.** Upstream switches slider attacks at avx2, move sorting at avx512 and threat writing at ICL, and this port follows; that makes this step the gate for a whole class of change, because it compares the vector path against the scalar path *on the same tree*. `signature` alone tests one tier and would pass over a wrong attack set at another. Run it on every ISA-gated commit. Not in `parity`: it is several full builds |
| `tb-cursed` | the DTZ > 100 cursed-win / blessed-loss battery plus two node-limited TB legs | the branches no 3-man table reaches. **LOCAL**, needs `./build.sh tb-fetch 5`, exits 127 without them — see [05-tablebases.md](05-tablebases.md) |
| `pgo` | instrument, profile the canonical `bench`, rebuild with `-fprofile-use` | nothing — it is a build mode, not a gate. Opt-in, mirroring upstream's separate profile build, so `build` and `parity` stay unprofiled |
| `perf-budget` / `perf-budget-update` | measure retired instructions against `tools/instr_budget.golden`, per ARCH | an instruction-count regression the node signature is blind to. **LOCAL**: needs `perf_event_open`, and the budget file is host- and toolchain-specific, so it is gitignored — a fresh clone reads 127 until someone records one |
| `sync-status` | compares the pinned SHAs against the tracked checkouts, in BOTH directions | that the pins are honest: a checkout *behind* its pin is red, a pin behind its checkout is a yellow report. Not a `parity` gate |
| `upstream-map` / `upstream-nodes` | the declared-map audit and uncovered ratchet; the random-position node differential | see *Resyncing the pin* below. **LOCAL** — both need the pinned upstream tree |
| `bench` / `clean` | run the benchmark; remove `build/` | nothing |
| `signature-update` / `golden-update` / `tb-cursed-update` | re-derive an anchor | read the warning below before running any of them |

`parity` runs, in this order: `build`, `zone-check`, `fmt`, `docs-lint`, `test`,
`signature`, `simd-scalar`, `perft`, `golden`, `tb`.

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

### Where a gate CAN make part of the distinction, it should

`tb-cursed-update` is the one update step that refuses. Its golden holds two
halves with different provenance: the WDL/DTZ probe results are oracle-derived,
and the two node-limited totals below them are a **self-golden** — the oracle
early-returns at depth 1 once the root is in a tablebase and reports `nodes 0`
for both legs, so it cannot supply them. Only the node legs may legitimately move
on an intended node-count change; a moved probe half is a prober bug.

So the step re-derives the node legs **only** when the probe half already matches,
and exits 1 without writing anything when it does not. That is not a substitute
for the judgement above — it just removes the one case where a mechanical check
can tell laundering from a stale value.

The general rule is the cheaper one: **a golden with no regeneration step rots**,
and it rots invisibly when it also sits outside `parity` and needs inputs the
default fetch does not supply — `tb_cursed.golden` is all three. Add the step that
re-derives a golden in the same commit that adds the golden.

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

The finish line does not move until the pin does. The pinned base is in
[`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE).

**No number for either appears anywhere in this documentation set**, and
`docs-lint` fails a page that quotes the anchor. Read it from the file, or run the
gate.

`./build.sh upstream-parity` builds a **pristine** upstream Stockfish at the pinned
SHA into a detached worktree outside the repo, runs both benches, and compares the
totals.

Pristine is the point. The oracle is upstream's own source built by upstream's own
Makefile, with no mcfish edit near it — a shared tree would let a bug present in
both cancel out and pass.

It is kept **out** of `./build.sh parity`: it needs a network fetch and a full
upstream build, which `parity` deliberately does not. Run it deliberately.

### Resyncing the pin

Advancing [`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE)
is a port campaign, not an edit, and two tools turn it from grep-and-hope into a
derivable plan. Both read the golden checkout (`../Stockfish`) and never touch
this tree.

- [`../tools/upstream_map.py`](../tools/upstream_map.py) derives the
  upstream-file → mcfish-owner map from the golden-reference citations the
  writing rules already require (mcfish renames every symbol, so the citations
  are the one join the rename boundary preserves). `--check` prints the coverage
  summary, the **uncovered** upstream files — unported surface, or ported code
  missing its citation, both debt — and the **phantom** citations naming files
  absent at the pin. Files not applicable by design carry their reason in
  [`../tools/upstream_map.exceptions`](../tools/upstream_map.exceptions); keep
  that list to true design decisions so the debt stays visible.
- [`../tools/resync_worklist.py`](../tools/resync_worklist.py) diffs the golden
  between the current pin and a candidate SHA and joins each changed upstream
  file to its owners: **change** rows are the re-port worklist ranked by churn,
  **absence** rows are new surface with no owner yet, **divergence** rows are
  owners holding retired code.

Two standing checks keep the map honest between resyncs, both run by
`./build.sh upstream-map` (LOCAL: it reads the pinned upstream tree from this
repo's git objects or the sibling checkout, which a CI clone does not carry):

- **The declared blast-radius audit.**
  [`../tools/upstream_map.tsv`](../tools/upstream_map.tsv) declares each
  upstream file's owners; the audit holds it to the derived reality. A derived
  owner missing from a row is DRIFT — the declared radius under-estimates the
  blast, the failure mode that let a sibling port route a whole subsystem's
  changes to two files. Absorb drift by widening the row; never trim a row to
  quiet the audit.
- **The uncovered ratchet.**
  [`../tools/upstream_map.baseline`](../tools/upstream_map.baseline) pins the
  uncovered upstream count. Lower it as citations land; never raise it. The
  two files above the floor today are the shm pair — the NUMA replication gap
  the report exists to keep visible.

The process: fetch upstream in the golden checkout, run the worklist against the
candidate SHA, port each row (every ported mechanism cites its upstream site, so
the map stays derivable), then move the pin, update the declared rows the
worklist touched, re-derive the anchor and every golden that legitimately moved,
and finish with `upstream-parity` and `upstream-map`. Cite with `file:line`
wherever a mcfish header shares the upstream basename — a bare shared name is
ambiguous and the map skips it rather than guess.

Three fidelity probes see three different bug classes, and each has caught one
the others cannot:

- the **anchor** catches a divergence that fires on the bench positions;
- **`upstream-nodes`** drives random-legal positions and catches one that never
  fires on any fixed list;
- a **node-limited suite run** (`bench <tt> 1 <N> default nodes` against the
  oracle's total) catches *cross-position warm-state* bugs — both engines
  bit-exact on every position in isolation while shared state (a TT generation
  counter, a persisting time-check counter) drifts across the suite. The
  bisection tool for this class is per-`go` checksums of each shared structure,
  compared side by side until one drifts.

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

Eight tools in `tools/` that are **not** `./build.sh` steps and **not** gates.
They measure the host they run on, and a shared, thermally-uncontrolled CI runner
cannot carry a performance verdict — so they are deliberately kept out of
`parity` and out of the workflows.

| Tool | Answers |
| --- | --- |
| [`../tools/nps_ab.sh`](../tools/nps_ab.sh) | **the headline speed ratio** — each engine's own search clock, interleaved, paired, order-alternating. The number that predicts Elo |
| [`../tools/perf_callgrind.sh`](../tools/perf_callgrind.sh) | deterministic instructions, D refs and cache misses — **sse41 only** |
| [`../tools/perf_counters.sh`](../tools/perf_counters.sh) | instructions AND cycles/IPC/cache-misses/branch-misses, on **every** arch tier |
| [`../tools/perf_sample.sh`](../tools/perf_sample.sh) | which SYMBOL burns the cycles — a `perf record` with no `perf`, every tier |
| [`../tools/perf_fingerprint.py`](../tools/perf_fingerprint.py) | per-function attribution, and the call-count parity test |
| [`../tools/perf_delta.py`](../tools/perf_delta.py) | startup-subtracted WORK counts (instructions, macro-ops) from `perf_counters` absolutes. Not for speed — see below |
| [`../tools/perf_counter_validate.c`](../tools/perf_counter_validate.c) | whether a counter counts what its name says, against two known bottlenecks |
| [`../tools/valgrind.sh`](../tools/valgrind.sh) | memcheck: invalid access, bad free, definite leak |

**Order of use, and the first three before any hypothesis.**

1. **`nps_ab.sh`** — is there a speed difference at all, and how big? It reads the
   engines' own search clocks, so there is no startup to remove and no arithmetic to
   get wrong. Ask this FIRST; a counter campaign that has not established the effect
   exists is a campaign against noise.
2. **`perf_fingerprint.py compare --calls`** — is it the algorithm? Call counts are
   inlining-immune, and a divergence there outranks every cost finding.
3. **an isolating workload** — `perft` for the board zone, `MCFISH_EVAL_MATERIAL=1`
   for the spine. Which component owns it.
4. **`perf_counters` + `perf_delta.py`** — only now, and only for the WORK axes.

Going straight to the counters is how a session spends a day attributing a deficit
that the first tool would have measured in five minutes — and, worse, how it reports
parity while a 13% gap sits in front of it.

`perf_counters.sh` drives both binaries interleaved over hardware counters
(`perf_event_open`, so the absent `perf` CLI does not matter), pinned to one core,
and reports the **median of per-round paired ratios**. It is the only tool here
that reads instructions on avx2 and native/vnni512 — where callgrind SIGILLs on the
AVX-512 EVEX prefix — and the only one that can *see* an IPC gap rather than infer
one.

It also reads **retired macro-ops** (AMD `ex_ret_ops`), which is the axis that says
whether an instruction-count difference is a difference in WORK. An x86 instruction is
not a unit of work — a folded load-op, a load-op-store and a wide vector op each retire
as one instruction and dispatch as two or more — so when the instruction and macro-op
columns disagree, the instruction ratio is measuring spelling and no conclusion drawn
from it survives. (On this repo's spine pair they agree: 1.015 vs 1.024 ops/instr, so
mcfish's instruction count really is its work.)

An IPC gap is USUALLY two things, and the tool reports both so the split is read
rather than guessed: **cache misses and branch misses**. When it is neither — as on
the spine comparison above — the tool has no answer, and the honest response is to say
so rather than to reach for a counter whose meaning has not been validated. A branch miss costs ~15–20
cycles and is invisible to the instruction count *and* to the miss rate, so a cycle
deficit that neither column explains is a prediction gap — and the only tool that can
attribute one to a call site is callgrind `--branch-sim=yes`, at sse41. For a change gated behind `__AVX512F__`, A/B the 128- and 64-lane native
binaries directly: the paired ratio cancels the thermal spread that makes a lone
cycles reading lie (a 128-lane transform that reads +3% cycles against the oracle in
one batch reads a flat 1.000 in a direct 12-round pair).

**Every ratio but instructions needs its own floor quoted beside it.** The five axes
do not share one: instructions is exact and the four efficiency axes spread over
more than an order of magnitude between tightest and widest, so one reading can be a
result on one axis and noise on another. The floors are **TBD** — they are a
property of the box's state at that moment, not of the host, and a control taken
right after a heavy build reads far wider than a settled one. Derive them next to
the comparison they floor, never once at the start of a session, and discard the
comparison when the control reads wide. See
[08-idiomatic-c.md](08-idiomatic-c.md#measurement-discipline).

**This tool counts the WHOLE PROCESS, including net load.** On a shallow bench that
is a large share of the total, and the whole-process ratio has been observed to
disagree in SIGN with the search-only ratio. Measure a near-empty search separately
and subtract it before quoting anything as a search result.

**Pick by size of the effect.** `nps` cannot resolve anything under about 5% —
wall-clock on this class of hardware swings by more than that between batches, so
it has both falsely confirmed and falsely refuted real changes. callgrind is
deterministic and resolves 0.01%. Use `nps_ab.sh` for the headline, callgrind for
anything smaller.

Tool-shape traps, each paid for:

- `perf_callgrind.sh` **prepends `bench` itself** — pass only the bench arguments.
  With `bench` passed twice the engine errors out after startup and the profile
  reads as a plausible startup-only run.
- `perf-budget` measures the **existing** `build/mcfish`. Rebuild at the target
  `MCFISH_ARCH` first, or the comparison crosses tiers and reads as a fake
  regression.
- Gates now rebuild when the binary is older than any source or header, so a stale
  binary can no longer answer. `./build.sh build` before a measurement is still worth
  running deliberately, because a rebuild inside a timed step leaves the machine hot.
- The hardware instruction counter is **blind to `rep stosb`** (an erms memset
  retires as one instruction) and callgrind is blind to software prefetch —
  memset and prefetch work need callgrind Ir and idle-box cycles respectively.

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
  it compares gcc with LLVM. Build a separate reference with clang for perf work,
  and VERIFY the version stamp matches on both sides before quoting any ratio —
  `readelf -p .comment <binary> | grep clang` must print the same version for
  mcfish and the oracle. An oracle binary of unknown provenance is not a
  baseline: one mislabeled build put a fake +9% tier regression into a standing
  table before the named-per-ARCH fresh-build rule existed.
- **Subtract startup.** On a shallow bench the net load, magic init and zero-fill
  are ~37% of the profile, and they are *cheaper* in mcfish than upstream — so
  the whole-process ratio reads 0.987x where the search-only ratio is 1.19x.
  Profile `printf 'quit\n' | <bin>` for a startup figure and subtract it, or name
  the offenders with `perf_fingerprint.py costs`.

### Two gates named parity, and only one reads the wire

`upstream-parity` compares one number — the bench total — and is the finish line for
the SEARCH. It says nothing about what the engine prints. `upstream-transcript`
compares the bytes, and is the only thing that can catch a divergence in the option
table, an `info string`, an error path or an exit code.

Both are needed and neither substitutes: a port can be bit-exact on nodes while
answering `setoption` with the wrong text, and it can print a perfect transcript while
searching a different tree. Keep `tools/transcript_known.txt` short — each line is a
standing claim that mcfish may differ from the golden, which is the opposite of what
this port is for, so each names what would retire it.

**Call counts, not costs, are the parity test.** `perf_fingerprint.py --calls`
answers "do we run Stockfish's algorithm?" — call counts are inlining-immune,
costs are not. Group on the symbols that exist in *your* build: clang inlines
upstream's affine layers into `Network::evaluate` while mcfish keeps
`nnue_affine_32` as a symbol, and upstream has two `do_move` overloads. A regex
written against the wrong side reads a divergence that is not there.

### Where the two engines stand on the SPINE, measured

The numbers below are startup-subtracted and bias-cancelled. Both corrections are
load-bearing and each one changed a published conclusion, so read the method before
the table.

**Subtract startup, by measurement and not by estimate.** `perf_counters` counts the
whole process, and startup is engine-dependent: mcfish parses the ~95 MB net in
roughly half upstream's time. On a bench short enough to iterate on that is a quarter
of the run, and the credit lands in every ratio. It cannot be corrected on the tool's
own output, because the difference of two ratios is not the ratio of two differences.
Run the pair over a deep workload and again at depth 1, and subtract the absolutes:
[`../tools/perf_delta.py`](../tools/perf_delta.py) reads the `#R` lines and does it.

**Run the pair both ways.** A paired ratio here carries a multiplicative position
bias of a couple of percent; `sqrt(fwd/swp)` cancels it exactly, and the A/A control
self-checks to 1.000.

Material-eval builds (`MCFISH_EVAL_MATERIAL=1` against an oracle patched with the
same formula, so the network is out of the picture and both engines walk one tree),
`x86-64-sse41-popcnt`, both through LLVM, `bench 16 1 13` minus `bench 16 1 1`:

| axis | mcfish | Stockfish | mc/sf |
| --- | ---: | ---: | ---: |
| instructions | 11.952e9 | 11.933e9 | **1.002** |
| macro-ops | 12.140e9 | 12.181e9 | **0.997** |
| cache misses | 19.27M | 20.47M | 0.939 |
| branch misses | 63.78M | 63.00M | 1.013 |
| cycles | 6.382e9 | 6.033e9 | **1.043** |
| IPC | | | **0.947** |

**The work is identical.** instructions 1.002 and macro-ops 0.997 retire the claim
this page used to make in two different tables — that mcfish executes 12–14% more
instructions per node (the per-tier table), or 11% fewer (the whole-process table).
Both were startup, sized by whatever share of the run startup happened to be, which is
why they disagreed in sign.

**The cycles row above is NOT a result, and the IPC derived from it is not either.**
It is left in the table because retracting it in silence is how it comes back. The
subtraction that produces it is leveraged in a way the instruction row is not: the two
deep runs differ by 1.5% while the startup quantities being removed differ by 67%
(mcfish 0.685e9 cycles against upstream's 1.142e9), so a small absolute error in either
startup term lands multiplied on the remainder — and cycles carry a 6–16% per-round
spread on this host. Instructions, being deterministic, are immune to that leverage,
which is exactly why the instruction row survives the method and the cycle row does not.

Measured WITHOUT any subtraction, on bench's own `Total time` — which both engines
start after the `ucinewgame` clear, and which therefore contains no startup by
construction:

| run | median mc/sf search time | spread |
| --- | ---: | ---: |
| depth 15, core 6, 14 rounds | **1.004** | 0.957–1.046 |
| depth 16, core 2, 8 rounds | **1.018** | 1.002–1.072 |

**mcfish IS slower per node, and the earlier parity claim on this page was wrong.**
It was measured on the sse41 non-PGO pair with an unreliable estimator, while the
games are played by the icl PGO pair. Measured properly — each engine's own `bench`
clock summed over a position set, interleaved, alternating which engine runs first,
9+ rounds, trees asserted identical:

| build | position set | depth | mc/sf | spread |
| --- | --- | ---: | ---: | --- |
| sse41, plain LTO | UHO book | 13 | 1.056 | 0.977–1.124 |
| icl, PGO | bench list | 13 | 1.097 | 1.061–1.119 |
| icl, PGO | bench list | 15 | 1.069 | 1.013–1.097 |
| icl, PGO | UHO book | 13 | 1.117 | 1.078–1.181 |
| icl, PGO | UHO book | 15 | 1.119 | 1.077–1.147 |

**7–13% slower per node at the shipping tier**, and the deficit roughly doubles from
sse41 to icl+PGO. The spreads at icl never touch 1.000.

Two protocol lessons, both of which produced a wrong published number here:

- **Measure the binaries that play the games.** A conclusion drawn on sse41 without
  PGO says nothing about icl with PGO; on this pair the deficit doubles between them.
- **Sum over a position set; never take a median of per-position times.** Search
  sizes across positions vary by orders of magnitude, so the median is dominated by
  which positions happen to land in the middle. The same binaries measured that way
  read 1.091 at depth 12 and 0.906 at depth 14 — a 20% swing between adjacent depths.
  `bench <tt> <threads> <depth> <fen-file>` sums, which is why it is the estimator to
  use, and it accepts a FEN file so any position set can be driven through it.

**What the deficit is not.** At equal nodes the two engines are behaviourally
identical — a 200-game fixed-node match returns Elo 0.00 ± 0.00 with Ptnml
[0, 0, 100, 0, 0], every pair a perfect mirror. Call-count parity is exact, symbol
for symbol. So the algorithm is right and the whole difference is the rate at which
each engine converts time into nodes.

**What the deficit IS: branch misprediction.** Bias-cancelled over both orientations
at the shipping configuration, against an A/A floor of 0.998:

| axis | mc/sf |
| --- | ---: |
| instructions | 0.895 |
| macro-ops | 0.880 |
| cache misses | 0.915 |
| **branch misses** | **1.382** |
| search time | 1.135 |

mcfish does 10% less work with better cache behaviour and mispredicts 38% more
branches — ~90 cycles per node against a ~1300-cycle node, the right size for the
whole gap. The trees are identical, so both engines' branches see the same decision
sequence: same data, same outcomes, worse prediction. That is layout and site count,
not logic, and callgrind's *ideal* predictor agrees — it puts mcfish at 1.008 where
the hardware says 1.382, and the difference between those two numbers is BTB and
history-table capacity, which callgrind does not model.

This names a defect class a transcription is structurally prone to: **where upstream's
templates emit N specialized copies, a port that emits fewer makes one branch site
carry the interleaved histories of several contexts.** `qsearch` was exactly that
until it was split. Upstream instantiates on NodeType, GenType, Color, PieceType and
several bools; every place this port serves two or more from one body is a candidate,
and each is testable alone by splitting it and re-reading the branch-miss ratio.

**Also attributed:** `-fno-unroll-loops`, applied at the 512-bit tiers for NNUE
I-cache reasons, costs the SPINE 2.0% (intra-engine, icl PGO, identical trees, median
0.980 over 11 alternating rounds). Whether it still pays with the network on is a
separate measurement; on a material-eval build it is pure cost.

### Strength testing, and what a match can actually resolve

**Read this before running games.** A cell that cannot resolve the effect you are
looking for does not return "no change" — it returns a number with a sign, and that
sign is a coin flip. Two runs of the SAME binaries at the SAME time control,
differing only in `-srand`:

| seed | Elo |
| --- | ---: |
| 20260728 | **−18.43 ± 17.50** |
| 991733 | **+2.78 ± 18.14** |

A 21-point swing from the opening set alone. Both were 1000 games.

The arithmetic that predicts this, and which decides the sample size BEFORE the run:

| games/cell | 95% CI (drawish pair) | resolves |
| ---: | ---: | --- |
| 200 | ±31 | almost nothing |
| 1 000 | ±18 | a large regression |
| 5 000 | ±8 | ~10 Elo |
| 10 000 | ±6 | ~6 Elo — a 6% speed change |
| 20 000 | ±4 | a 4% speed change |

Speed converts at roughly **70 Elo per doubling**, so a 6% per-node gain is about
+6 Elo and needs ~10 000 games to see. Twelve 1000-game cells were run here against
an effect that size; none of them could have detected it, and the differences
*between* cells were read as structure when they were the opening set. **Never
compare two cells that each carry a ±18 bar.**

Corollary: for a few-percent change, `tools/nps_ab.sh` is not a weaker substitute for
an Elo run — it is the stronger measurement, because its spread can exclude 1.000 in
nine rounds where the match needs ten thousand games.

**A fixed-NODE match is a diagnostic, not a strength test.** Give both engines
`nodes=N` and any difference in play is impossible unless the search itself differs.
Between this port and its oracle it returns **Elo 0.00 ± 0.00, Ptnml [0, 0, 100, 0,
0]** — every pair a perfect mirror. That single run proves the search, the evaluation
and the move ordering are identical, which localises every timed-game difference to
how many nodes each engine chooses to spend. Run it whenever a behavioural
divergence is suspected; it is 200 games and answers a question no counter can.

**Isolate the evaluation when the question is about the spine.** Build both sides
with the material eval (`MCFISH_EVAL_MATERIAL=1` here, the matching patch on the
oracle) and assert the node counts match before believing anything — this catches the
build that silently came out with the network still in it, which happened here when
`EXTRACXXFLAGS` did not reach a sub-make and would otherwise have made the whole
match measure evaluation instead of speed.

### Debugging a divergence the gates cannot see

Every gate in this repository is behavioural: it compares what the engine *does*.
A divergence in how the engine is *laid out* or *allocated* passes all of them —
identical call counts, identical node counts, green signature, perft, golden and tb.
Four of the six found this way were invisible to every gate here.

The method, in the order that worked:

1. **`sizeof` every hot structure against the oracle.** A C program and a C++ program
   printing the same list side by side. `Position` read 744 bytes against upstream's
   1056, and the 312-byte gap named a missing member (`castlingPath`) directly.
2. **`offsetof`, field by field, when the sizes match.** Equal size does not mean
   equal layout. `StateInfo` matched at 184 bytes with `previous` at 176 against
   upstream's 80 — same fields, different cache line, on a pointer chase run a
   million times a search.
3. **`/proc/PID/maps` for every large allocation.** Alignment and huge-page backing
   are invisible to every other tool. The 256 MB table sat at 4 KiB alignment against
   the oracle's 2 MiB.
4. **`grep` upstream for its ISA gates**, not its portable path — see
   [08-idiomatic-c.md](08-idiomatic-c.md#port-upstreams-isa-gated-paths-not-just-its-logic).
5. **`perf_fingerprint.py compare --calls`** to confirm the algorithm is unchanged
   while you do all of the above.

### A counter is a hypothesis until it is validated

Two conclusions in this repository have been drawn from an event whose documented
name did not describe its behaviour on this host. Both were wrong, and the second was
reported as a finding before being checked.

[`../tools/perf_counter_validate.c`](../tools/perf_counter_validate.c) is the check:
two loops whose bottleneck is known from first principles — one serial dependency
chain (latency-bound, IPC pins near 1) and four independent chains (throughput-bound,
IPC 3+). If a counter does not move the way the bottleneck demands, it does not mean
what its name says.

Worked example, and the reason those two events are **not** in `perf_counters`:

| loop | IPC | "front-end starved" | "back-end stalled" |
| --- | ---: | ---: | ---: |
| serial chain (latency-bound) | 1.00 | 8M | **1M** |
| independent ILP (fast) | 3.30 | 57M | **1775M** |

The textbook latency-bound loop reads essentially zero on the back-end column and the
FAST loop reads 1775M, because the events count **dispatch** pressure rather than
execution: in the chain each op dispatches at once and then waits in the scheduler,
which is not a dispatch stall. A higher back-end number means the front end is running
further ahead, which accompanies fast code as readily as slow. They were briefly wired
into `perf_counters`, produced two wrong findings, and were removed rather than
documented — a tool should not offer a foot-gun whose only record is self-harm.

Two arithmetic checks would have caught it without the microbenchmark, and both are
worth running on any new counter: do the parts sum to the whole (here the two deltas
netted to +3M cycles against a measured +212M gap), and does the derived quantity have
a sane magnitude (dispatched-ops + starved + stalled came to 3.56 slots/cycle for one
engine and 3.69 for the other, on a 6-wide machine).

### Isolate the component instead of attributing it

Two workloads answer questions no profile of the full engine can, and neither needs an
attribution argument because each simply removes what it is not measuring:

- **`perft`** is the board zone — movegen, make/unmake, legality, threats — with no
  TT, no histories, no move ordering and no evaluation. It reports its own node count,
  so tree identity is checked for free.
- **`MCFISH_EVAL_MATERIAL=1`** replaces the evaluation with a material sum, which
  leaves the spine and the search running over a tree the network no longer shapes.
  Patch the oracle with the same formula — the weights are written out in both, not
  read from either engine's tables, precisely so the two cannot drift — and assert the
  node counts match before believing anything. They do: 627773 at `bench 16 1 8`,
  2284915 at `16 1 12`, 10104475 at `16 1 15`.

Running the same comparison over both is what localised the IPC gap to the search
rather than the board, in two commands and with no per-function attribution at all.

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
