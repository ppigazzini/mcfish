# Tooling and CI

Every `./build.sh` step and what it actually gates, the two source arrays that
decide what is gated at all, the golden-diff harness and what its normalization
throws away, the two kinds of expected-value file, the anchor versus the finish
line, and the CI lanes.

Audience: all developers. The workflow around these gates is in
[`../CONTRIBUTING.md`](../CONTRIBUTING.md). Every gate here decides a VALUE; the
instruments that decide a COST are [11-performance.md](11-performance.md)'s, and
none of them blocks a merge.

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

No file is in that state today, and that is now gated rather than re-checked by
hand: `zone-check` compares `find src -name '*.c'` against both arrays before it
links anything, so a file in no array, or an `engine/`/`platform/` file outside
`ENGINE_SOURCES`, is red. It also refuses an `ENGINE_SOURCES` entry from `shell/` —
including one would supply the very symbol whose absence is the proof — and floors
the file count, so a `find` that answers nothing fails instead of passing over the
empty set. Two consequences for anyone using these gates:

- **Adding a file means editing `SOURCES`**, and `ENGINE_SOURCES` too if it belongs
  to `engine/` or `platform/`; `zone-check` names the file if you forget.
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

### One file, and no size gate — a standing decision, not an oversight

`build.sh` is 1983 lines and 57 functions, and it stays one file. The property
being bought is that the build and every gate are a single self-contained script:
no source-path bootstrap, no half-installed fragment directory, and a copy of this
one file plus the tree builds and gates the engine.

**The sibling drew the opposite conclusion, and the contrast is the useful part.**
`../zfish` holds every file to 500 lines under a ratcheting `loc` gate — a baseline
that may fall and never rise — and when its `build.zig` reached 2595 lines it split
into a `build/` package rather than raising the baseline (zfish `eeb52780`,
`5ffd1bca`). That works there because the build file is Zig, checked by the same
lint as the program. Here the equivalent policy is absent entirely: **there is no
file-size gate in this tree at all**, and 11 files under `src/` are already over
500 lines (`nnue_accumulator.c` at 1365 is the largest), so importing the rule
would be a refactor campaign rather than a gate.

What the decision costs, and what to watch instead:

- **Navigation is by `grep`, not by file name.** The dispatcher's `case` block is
  the index; `./build.sh help` is the user-facing one.
- **One tool still reads this file as TEXT** — `docs_lint.sh` extracts the step
  list from the last `case` block, and guards its own extraction so it fails at
  exit 2 rather than going quietly vacuous. There were two: the audit lifted
  `normalize()` out with a `sed` until that function moved to
  [`../tools/lib/normalize.sh`](../tools/lib/normalize.sh), which both callers now
  source. **That is the cheap half of a split, and the half that was actually
  paying for itself**: the extraction hack existed because the build had no
  importable unit, and one shared file removed it without moving the other 1900
  lines. Anything that grows a text dependency on `build.sh` should be asked the
  same question — can it source instead? — before it grows a guard.
- **A split, if it is ever wanted, has to prove equivalence rather than assume it.**
  The check zfish used is the one to reuse: diff the full step-name list before and
  after, because a step can stop existing while `build`, `parity` and every gate
  stay green — that is how it found two deleted steps and one silently dropped
  argument.

## The gates

[`../build.sh`](../build.sh) is the whole build system and the whole in-repo gate
battery, so this page's gates section is the WHOLE list rather than one page's
share of it — which is why it sits here rather than at the end.

`./build.sh help` prints the steps; this table says what each one *proves* and
where it is described. **A step whose subject is one zone is described on that
zone's page**, under that page's own `## The gates` section, and routed to from
the last column here; `this page` means the mechanics are below. Every page in the
set carries such a section, and `docs-lint` holds all of them to each other in
both directions — a step in no page's table, and a row pointing at a page that
does not carry the step, are both findings.

| Step | What it does | What it gates | Described in |
| --- | --- | --- | --- |
| `build` | clang `-O3 -DNDEBUG`, one invocation over `SOURCES` | that the files **in `SOURCES`** compile under the full warning set. Not the tree — see above. | [08-idiomatic-c.md](08-idiomatic-c.md) |
| `debug` | the same sources with ASan + UBSan and `-fno-sanitize-recover=undefined` | nothing on its own; it is the binary the sanitizer lane drives. | [08-idiomatic-c.md](08-idiomatic-c.md) |
| `zone-check` | checks both arrays against `find src -name '*.c'`, then links `ENGINE_SOURCES` plus a stub `main`, with no shell object | that no `engine/` file calls into `shell/`, over the whole zone rather than over whatever the array happens to name. | [00-architecture.md](00-architecture.md) |
| `engine-standalone` | compiles every `src/engine/*.c` alone and links them with **no** platform object | the engine→platform edge, as a ratcheted count against [`../tools/engine_platform.baseline`](../tools/engine_platform.baseline). | [00-architecture.md](00-architecture.md) |
| `test` | builds `ENGINE_SOURCES` + [`../tests/test_main.c`](../tests/test_main.c) under ASan+UBSan and runs it, with `-DMCFISH_ACC_STATS` | the unit and property suite: perft to reference counts, make/unmake round-trip, incremental-vs-recomputed Zobrist, search determinism, the accumulator's four update paths, each asserted twice — see [03-engine-eval.md](03-engine-eval.md) — and three randomised walks, each seeded and fixed so a failure reproduces: 200 searches from positions reached by random legal moves — the only always-run gate that enters the node body from an arbitrary board, and the only one whose searches evaluate through the network at all, since every other search in the suite runs before the net is loaded; 200 lines of up to 60 plies compared key-for-key against a from-scratch parse at every ply and then unwound to the root, which is where the states a depth-3 tree never reaches live; and 4000 mutated and random FEN strings, where rejection is never a failure but every string ACCEPTED must generate moves and render a FEN that parses back to the same board | this page |
| `fuzz-search [seconds]` | builds `ENGINE_SOURCES` + [`../tools/fuzz_search.c`](../tools/fuzz_search.c) under libFuzzer+ASan+UBSan and runs it for the given budget (default 30s) | the real search **in-process**, with no shell and no seam registered. clang-only, out of `parity`. | [02-engine-search.md](02-engine-search.md) |
| `fuzz-tb [seconds]` | builds **two** libFuzzer+ASan+UBSan drivers and runs each for the given budget (default 30s): one over `decode_set_sizes`/`decode_pairs` directly, one over a real file through `tablebase_init` | the Syzygy parse, in two lanes neither of which subsumes the other. clang-only, out of `parity`, same reasons as `fuzz-search`. | [05-tablebases.md](05-tablebases.md) |
| `tsan` | rebuilds `ENGINE_SOURCES` + the test binary under ThreadSanitizer and runs it | the thread pool's happens-before edges. Out of `parity`: its own build, roughly triples the suite. | [04-multithreading.md](04-multithreading.md) |
| `tsan-search [depth] [threads]` | builds the **whole engine** under ThreadSanitizer and drives one `go` through the UCI front end | races in the SEARCH, which `tsan` cannot see, at a peak thread count it samples rather than assumes. | [04-multithreading.md](04-multithreading.md) |
| `signature` | runs the default `bench` (a bare `engine bench` — the full position list at depth 13, `Hash 16`, one `ucinewgame`), compares the node total to [`../tools/signature.golden`](../tools/signature.golden) | that no edit changed search behaviour unintentionally | this page |
| `net-roundtrip` | drives `export_net` and compares the exported file with the net in `resources/`, byte for byte | the .nnue **writer**, which every other gate is structurally blind to — nothing else in the battery reads what the engine WRITES. | [03-engine-eval.md](03-engine-eval.md) |
| `speedtest-check` | drives `speedtest 1 8 1` and reads the report back | the `speedtest` command, every number in which is a property of the machine and so can never be a golden. | [07-shell.md](07-shell.md) |
| `perft` | drives every row of [`../tools/perft.table`](../tools/perft.table) through the UCI front end | move generation totality, against counts that are facts about chess rather than a golden. | [01-engine-board.md](01-engine-board.md) |
| `golden` | diffs each `tools/cases/*.uci` transcript against its `.golden` | the observable UCI surface, byte for byte after normalization. | [07-shell.md](07-shell.md) |
| `tb-fetch` | downloads the 3-man Syzygy set (KPvK KNvK KBvK KRvK KQvK, WDL+DTZ) into `resources/syzygy/` | nothing — it *fetches*, and verifies each file's Syzygy magic. | [05-tablebases.md](05-tablebases.md) |
| `tb` | runs the discovery report and the root probe battery in [`../tools/cases/tb.fens`](../tools/cases/tb.fens), diffed against [`../tools/tb.golden`](../tools/tb.golden) | Syzygy discovery, the root DTZ/WDL ranking and the probe path. **Without the tables it checks discovery only and says so in red.** `tb-update` re-derives that golden from the ORACLE. | [05-tablebases.md](05-tablebases.md) |
| `tb-update` | re-derives [`../tools/tb.golden`](../tools/tb.golden) by running **the oracle**, and refuses without the full table set | nothing — it re-derives. There is no mcfish-derived path to that golden at all | [05-tablebases.md](05-tablebases.md) |
| `fmt` / `fmt-fix` | `clang-format --dry-run --Werror` over `src/` and `tests/` | formatting. Exits **127** when no `clang-format` is found. | [08-idiomatic-c.md](08-idiomatic-c.md) |
| `docs-lint` | [`../tools/docs_lint.sh`](../tools/docs_lint.sh) over every tracked `*.md` | dead internal links, named paths that do not exist, a quoted bench signature, a vanished symbol, and a `build.sh` step no page mentions — with two floored extractions, so a stale pattern fails at 2 instead of reporting OK over nothing. | [13-writing.md](13-writing.md) |
| `type-check` | `tools/type_cases/*.c` compiled with `-fsyntax-only` and **`CFLAGS_COMMON` itself** | that every refusal 09-type-design.md claims is still refused, and every legal form beside it still compiles. It is the gate on the `-Werror=` promotion list, not on the headers. | [09-type-design.md](09-type-design.md) |
| `cite-check` | [`../tools/docs_cite.sh`](../tools/docs_cite.sh) over every backticked 7–12 digit hex token in a tracked `.md` | that a cited commit SHA still names a commit a reader can reach. **Ancestry, not existence**, in four tiers. | [13-writing.md](13-writing.md) |
| `shellcheck` | [`../tools/shellcheck.sh`](../tools/shellcheck.sh) over every `.sh` the index tracks, at severity `style` | the defect classes shellcheck knows, in the language the gates themselves are written in. Held at **zero findings with no baseline** — a suppression is a `# shellcheck disable=` at the site with its reason beside it, because the findings here are cheap enough to fix that a register would be the only debt list that could never expire. The version is pinned in **two fields** (`tools/shellcheck.version`): the `shellcheck-py` package version and the binary version it ships, which are not the same number. Resolved through `uvx`, as `ruff` and `ty` already are; exits **127** when neither a matching binary nor `uvx` is reachable | this page |
| `upstream-parity` | [`../tools/upstream/upstream_parity.sh`](../tools/upstream/upstream_parity.sh) | mcfish's bench against a pristine upstream build — see below | this page |
| `golden-audit` | drives every `tools/cases/*.uci` script through a PRISTINE upstream build and diffs against the committed golden | that each golden is upstream's bytes rather than a photograph of mcfish. **LOCAL**. | [07-shell.md](07-shell.md) |
| `fingerprint` | profiles both engines under callgrind on one tree and asserts each group in [`../tools/fingerprint_groups.tsv`](../tools/fingerprint_groups.tsv) is CALLED as often here as upstream | the ALGORITHM, which every other differential is blind to. **LOCAL**, ~50x slow, not in `parity`. | [11-performance.md](11-performance.md) |
| `upstream-transcript` | drives both engines over [`../tools/cases/transcript/`](../tools/cases/transcript) and diffs the whole output | the UCI surface against the ORACLE, which `golden` structurally cannot do. **LOCAL**. | [07-shell.md](07-shell.md) |
| `parity` | the aggregate | the ten gates listed below it — every in-repo gate, but not `upstream-parity` | this page |
| `net` / `net-fetch` | name the `.nnue` this build expects and report whether it is present; fetch it into `resources/` and **sha256-verify** it | nothing — one reports and one fetches. The split is what keeps `build` off the network. | [03-engine-eval.md](03-engine-eval.md) |
| `simd-scalar` | rebuilds with `MCFISH_SIMD_SCALAR` — every vector type and intrinsic compiled out — and re-asserts the anchor | that `simd.h`'s two implementations are value-identical. In `parity`. | [03-engine-eval.md](03-engine-eval.md) |
| `lane-coverage` | every step `build.sh` dispatches must appear in a workflow, in `parity`, or in an excused list with a reason | that **a lane in no gate is not a lane** — a rule that was enforced by somebody remembering it until four differentials quietly stopped being lanes, `upstream-parity` (the finish line) among them. The excused list is the hole, so it expires in its own direction: a step excused that *does* run is reported as a stale excuse. In `parity` | this page |
| `golden-coverage` | globs `tools/*.golden` from the tree and holds each to a reader — a case that diffs it, or an owner row that names it | that a golden nobody diffs is a file, not a check. In `parity`. | [07-shell.md](07-shell.md) |
| `tools-smoke` | runs every tool that no other lane invokes and asserts it still prints the interface its callers read | that a tool nobody runs has not rotted. It had: `valgrind.sh`'s header claimed the search was single-threaded and `Threads` accepted and ignored — false since Lazy-SMP landed, and it survived because nothing ran the file | this page |
| `counter-validate` | drives [`../tools/perf_counter_validate.c`](../tools/perf_counter_validate.c)'s two loops under the counter harness and asserts the **separation** | that a counter means what its name says on *this* host. **LOCAL**. | [11-performance.md](11-performance.md) |
| `async-check` | drives a REAL interrupted search and asserts ten invariants over `stop`, `quit`, a mid-search `setoption`, `export_net`, `go movetime 0` and a critical error | the interrupted-search path, which no byte-golden can hold. **LOCAL**, ~6s | [07-shell.md](07-shell.md) |
| `fixture-coverage` | holds [`../tools/fixture_properties.tsv`](../tools/fixture_properties.tsv) to the tree, in both directions | that the input domain is written down. In `parity`. | [07-shell.md](07-shell.md) |
| `negative-control` | mutates the engine once per gate — a razor margin, the `d` command's `Checkers:` line, the knight under-promotion — and requires that gate to exit non-zero, then restores and requires it to pass | that a gate can FAIL. Every other gate's detection power is an assumption until something breaks the engine on purpose; two gates this month turned out to be incapable of failing at all. Both rig faults are distinguished from verdicts: a pattern that matches nothing exits 2, and a mutant that outruns `NEG_GATE_TIMEOUT` exits 2 rather than being credited as a detection. One row IS held (see below) and the default run greps its search text without building, because a held row rots in silence otherwise. **LOCAL**, ~100s for the engine rows and about as much again for the four added since; the `simd-scalar` mutant had to be bounded first (see below) | this page |
| `arch-determinism` | builds every ISA tier the host can execute and requires one node count | that the evaluation is arch-invariant, and — since the tiers now run different ALGORITHMS — that those algorithms agree. Not in `parity`: several full builds. | [08-idiomatic-c.md](08-idiomatic-c.md) |
| `malformed` | two families past the sanitized engine — crafted `.rtbw` headers that must be REFUSED, and real tables with a few bytes changed that must be ABSORBED — plus two controls that must stay silent | that a file refused yesterday is refused today, which neither `signature` nor the nightly fuzz lane can be. 2.4 s, in `parity`. | [05-tablebases.md](05-tablebases.md) |
| `attribution` / `attribution-update` | diffs the tail of [`../AUTHORS`](../AUTHORS) — everything past its header rule — against upstream's own `AUTHORS` at the pin, read straight out of the golden's object store | that the GPL attribution mcfish is REQUIRED to carry is still upstream's list, which nothing else here reads: no node count, no golden and no lint opens this file. It had drifted by six names across four syncs, one of them added by `22dfb404` in the range the port that added this gate had just closed — a resync reads `src/`, so a commit touching `AUTHORS` and `search.cpp` together gets ported by its diff and silently loses the half that is not code. **NARROWS** to unchecked without a golden checkout, the way `tb` does, so it can sit in `parity` and still be the real comparison in the weekly upstream lane. `attribution-update` re-derives the copy. | this page |
| `tb-cursed` / `tb-cursed-update` | the DTZ > 100 cursed-win / blessed-loss battery plus two node-limited TB legs | the branches no 3-man table reaches. **LOCAL**, needs `./build.sh tb-fetch 5`, exits 127 without them. | [05-tablebases.md](05-tablebases.md) |
| `pgo` | instrument, profile the canonical `bench`, rebuild with `-fprofile-use` | nothing — it is a build mode, not a gate. | [08-idiomatic-c.md](08-idiomatic-c.md) |
| `perf-budget-tb` / `perf-budget-tb-update` | the same measurement over a PROBING workload, filed under a `<tier>+syzygy` row | the tablebase reader, which `perf-budget` cannot see at all. **LOCAL**, needs `perf_event_open` *and* `./build.sh tb-fetch 5`. | [11-performance.md](11-performance.md) |
| `perf-budget` / `perf-budget-update` | measure retired instructions against `tools/instr_budget.golden`, keyed by the ISA TIER in the binary and held to 0.05% | an instruction-count regression the node signature is blind to. **LOCAL**, and the budget file is gitignored. | [11-performance.md](11-performance.md) |
| `perf-decomp <base> <head>` | callgrind over both binaries, SELF cost per symbol, grouped by [`../tools/perf_components.tsv`](../tools/perf_components.tsv) | where a cost difference actually is, deterministically to the instruction — attribution by hand across two differently-inlined binaries is void by construction. **LOCAL** | [11-performance.md](11-performance.md) |
| `material-eval` | patches the evaluation down to material and rebuilds | nothing — it is the ablation that prices the spine and search with the network gone. **LOCAL** | [11-performance.md](11-performance.md) |
| `sync-status` | compares `UPSTREAM_BASE` against the golden checkout, in BOTH directions | that the pin is honest: a checkout *behind* the pin is red (every grep of it then answers from source already ported past), a pin behind its checkout is a yellow report. Tracks the golden ONLY — `../zfish` is a sibling port with no pin here, see [`../tools/upstream/README.md`](../tools/upstream/README.md). Not a `parity` gate | this page |
| `upstream-map` / `upstream-nodes` | the declared-map audit and uncovered ratchet; the random-position node differential | see *Resyncing the pin* below. **LOCAL** — both need the pinned upstream tree | this page |
| `bench` / `clean` | run the benchmark; remove `build/` | nothing | this page |
| `help` | prints every step `build.sh` dispatches | nothing — it *reports*, and `lane-coverage` is what holds it to the dispatch table | this page |
| `signature-update` / `golden-update` / `tb-cursed-update` | re-derive an anchor | read the warning below before running any of them | this page |

### The gates are written in a language nothing was reading

`build.sh` is over three thousand lines of hand-written bash and it decides every
claim this tree makes; `tools/` holds fifteen more scripts that do the deciding. The
pre-commit config lints, formats and type-checks the Python and `fmt` covers the C.
Until `shellcheck` landed, the shell had nothing.

**What it proves is narrow, and the narrowness is the point.** shellcheck reads
syntax and idiom. It cannot tell whether a gate checks the thing it claims to —
that is `negative-control`'s job, and the two are not substitutes: a script can be
shellcheck-clean and assert nothing at all. What it does catch is the class one
level below, where `do_fmt`'s own comment already names the failure mode — a gate
invoked as the left operand of `||` runs with `set -e` disabled for its whole body,
so a missing `|| return 1` prints "all gates passed" over real violations.

Its first run found thirty findings, and two were defects rather than idiom: five
`cd` calls in gate scripts that would have run the rest of the script in the wrong
tree had the `cd` failed, and a `A && B || C` reporting line that prints **both**
verdicts if `B` ever returns non-zero. The rest were quoting and a dead local.

**Pin the version, in both fields.** A lint's finding set is version-dependent —
0.9.0 reports a trap-invoked cleanup as `SC2317` and 0.11.0 as `SC2329` — so a
suppression written against one version is not a suppression under the other.
refish records running its equivalent unpinned and getting 0 findings on one box and
65 on another from the same tree, which is the most expensive false positive a lint
can produce: the gate goes red and nobody changed a script.

### The mutant must be bounded, not merely wrong

`negative-control` is no longer one row per gate. Three gates carry two, because one
mutant can only reach one half of what they claim:

| gate | rows |
|---|---|
| `malformed` | delete the refusal diagnostic; unbound the decoded symbol. The first reaches the crafted-header family, the second the mutated-table family — and only the second proves those fixtures reach the DECODE loop rather than the load. **The second is HELD**: the family that detects it needs the 3-man corpus, and on a machine without it the gate narrows and stays green, which this rig would credit as "passed a mutated engine" — a verdict about the machine, not the code. Run it with `./build.sh tb-fetch && ./build.sh negative-control malformed` |
| `async-check` | the two OPPOSITE edits of one line: never stop an unbounded search (the wedge), and stop every search including bounded ones (the truncation that the obvious fix for the wedge causes). A gate that pins only one of those licenses the other |
| `test` | sign-extend `hash_bytes`' tail again |

A gate whose claim has two directions needs a mutant in each. The `async-check` pair
is the clearest case: both mutations are one-line edits of the same condition, both
plausible as a refactor, and each is invisible to the row that catches the other.

### A held row rots in silence, so its pattern is checked without being applied

The held `malformed` row named the decoder's per-symbol bound. When that check was
proved at load time instead and the line deleted, the row kept naming the line that
was gone — through two full `parity` runs, both of which reported `9 of 9` and were
telling the truth, because a held row is outside the denominator as well as the
numerator. The rot surfaced only on an explicit `./build.sh negative-control
malformed`, and then as a **rig fault** rather than a verdict.

That is the failure mode a hold creates: the run that would notice is the run nobody
makes. The default run now greps each held row's search text against its file and
fails if the tree no longer carries it — a grep, no build, and it turns "this row is
not run today" into "this row could still run today". It does not check that the
mutation still *means* what the row claims; only that it can still apply.

The `simd-scalar` row is the one that taught the bounding rule. Its first mutant inverted the scalar activation clamp (`min`
becomes `max`), which hands the search an evaluation with **no ceiling** — so the
mutated engine searched a tree that does not converge. The clean gate takes ~90s; the
mutated one ran past **900s** twice, once for over 25 minutes, without returning a
verdict. The gate would have been right. It never got to say so.

That is why a timeout here is a **rig fault** and never a detection: crediting a gate
for an experiment that did not finish is the same defect as reporting a comparison
that was never made.

The replacement shifts the scalar right-shift one bit further, which **scales** the
evaluation instead of unbounding it. The engine stays a sane chess engine searching a
different tree, the scalar build benches a different total, and the gate reddens in
35s. The rule generalises to any mutant aimed at an evaluation: **perturb the value,
do not remove the bound.**

`parity` runs, in this order: `build`, `zone-check`, `fmt`, `docs-lint`, `shellcheck`,
`cite-check`, `type-check`, `fixture-coverage`, `lane-coverage`, `golden-coverage`,
`tools-smoke`, `test`,
`signature`, `net-roundtrip`, `speedtest-check`, `simd-scalar`, `perft`, `golden`, `tb`.

`tools/tb.golden` is **oracle-derived**: `./build.sh tb-update` regenerates it by
running the pristine upstream binary over the same battery, never mcfish. It pins
each position's root `score` and `tbhits` from the **depth-1** info line and
deliberately pins neither nodes, pv nor bestmove — upstream early-returns at depth
1 once the root is in the tablebase while mcfish searches on, and among
equally-optimal TB moves either may pick a different winning one. Gating that
would be fake parity. See [`../tools/GOLDEN_PROVENANCE.md`](../tools/GOLDEN_PROVENANCE.md).

### A skipped gate is not a passing gate

`fmt` exits 127 when `clang-format` is absent, and `signature`, `net-roundtrip` and
`simd-scalar` do the same when no NNUE net is reachable. `parity` treats each as *skipped*, keeps
going, and then **names every skipped gate in its summary line** — because
"parity passed" printed over a silently absent linter, or over an anchor nobody
actually checked, is exactly how a gate rots into decoration. Those three are the
gates here that can be skipped; every other one runs on a bare toolchain with no
net.

**Narrowing is a third state, and it is not skipping.** A skipped gate ran
nothing; a narrowed one ran what its inputs allowed and says which half it could
not reach. `tb` does this without the Syzygy corpus — discovery only, announced in
red — and `malformed` does it the same way, running its crafted-header family and
an empty-path control while reporting the mutated-table family unexercised. The
distinction earns its keep in both directions: a gate that FAILS for a missing
optional resource takes the aggregate down with it for a reason unrelated to the
change under test (`malformed` did exactly that to the blocking lane, exiting 2
on every CI run for want of 26 KB of tables), and one that quietly passes over the
same absence is decoration. Narrowing is how a gate stays honest about a resource
it does not control — but only if what remains still asserts something, which is
why `malformed` keeps a control that needs no tables rather than trading its
control for an exit code.

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
position — so they do not depend on this engine, on its evaluation, on its search,
or on the port's progress. A mismatch is always a move generation bug and never an
update candidate. The rows, the variant field and what the table is blind to are
[01-engine-board.md](01-engine-board.md)'s, beside the generator they hold.

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

Neither compares a COST. The instrument that asks whether the two engines run the
same ALGORITHM is `perf_fingerprint.py --calls`, and it is
[11-performance.md](11-performance.md)'s.

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

Three things about that process are easy to get wrong and cost real time:

- **Land one upstream commit per mcfish commit, in upstream order, and check each
  behaviour-changing one against upstream's OWN `Bench:` line at that commit.**
  Upstream states the anchor in the commit message of every functional change, so
  the range hands you a checkpoint per step instead of one at the end. A sync
  landed as a single commit has exactly one place to be wrong and no way to bisect
  it; one that follows upstream's order has a green anchor at every step.
- **Build the oracle at the commit under test, not at the old pin.**
  `upstream_oracle.sh <sha>` takes one, and it is what turns "our number equals the
  number in the message" into a differential against a pristine build.
- **`tools/tb.golden` is EVAL-DEPENDENT and must be re-derived from an oracle at
  the matching commit.** Its root-probe rows carry PVs, so a net or search change
  moves them. Re-deriving it from mcfish would silently convert that gate from a
  differential against upstream into a snapshot of whatever mcfish currently does.
  `tb-cursed` splits the same way on purpose: its WDL/DTZ half is the
  oracle-pinned one and must be green *before* `tb-cursed-update` re-derives the
  node-limited legs.

A commit in the range with no counterpart here is a result, not a gap to skip
over silently. Say which commits those were and why in the pin-advance commit
body — a backend this port does not carry, a C++ construct with no C23 analogue, a
facility whose base was never taken — so the next resync does not re-derive the
same nine answers.

Three fidelity probes see three different bug classes, and each has caught one
the others cannot:

- the **anchor** catches a divergence that fires on the bench positions;
- **`upstream-nodes`** drives random-legal positions and catches one that never
  fires on any fixed list. It refuses to run at all — `net_identity_or_die` —
  if the two engines it drives loaded different nets (or one loaded none):
  that failure mode reads identically to a catastrophic search bug otherwise,
  and reporting node diffs under it would be worse than not running;
- a **node-limited suite run** (`bench <tt> 1 <N> default nodes` against the
  oracle's total) catches *cross-position warm-state* bugs — both engines
  bit-exact on every position in isolation while shared state (a TT generation
  counter, a persisting time-check counter) drifts across the suite. The
  bisection tool for this class is per-`go` checksums of each shared structure,
  compared side by side until one drifts.

## CI

Four workflows in [`../.github/workflows/`](../.github/workflows). None of them
does anything a developer cannot reproduce with `./build.sh` (or, for the
upstream-check lane, `tools/upstream_map.py` directly); anything that diverges is
a bug in the workflow file.

### `mcfish_parity.yml` — the blocking lane

Runs on every push and PR, with four jobs:

- **`fmt`** — split out and run first because it is the cheapest signal. It
  duplicates the `fmt` inside `parity` on purpose: whitespace drift caught in a
  minute beats whitespace drift caught fifteen minutes in. clang-format is
  installed at the **same pinned major** as the compiler, because a different
  major reflows code that was clean under another one and the gate would flap.
- **`parity`** — `./build.sh parity`, after asserting the installed clang is new
  enough for the C23 the tree uses. The pin is explicit so a toolchain regression
  is attributable to a commit rather than to a floating runner image.

  It fetches two RESOURCES first, and both are best effort by design: the NNUE net
  (cached; without it `signature`, `net-roundtrip` and `simd-scalar` skip) and the
  26 KB 3-man Syzygy set (without it `tb` and `malformed` narrow). Neither is this
  project's dependency to be held hostage by, so a mirror that is down costs
  coverage the run announces rather than a red blocking lane. What that buys is
  the difference between a lane that is green and one that checked what it claims:
  before the tables were fetched here, `tb` had only ever asserted discovery in
  CI.
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

  **It is a matrix over two pinned versions, `gcc-13` and `gcc-14`, not the
  runner's default.** The default moved from 13 to 14 underneath this workflow
  while its own comment still described gcc-13, so the lane was not testing what
  it claimed. The two rows are not redundant: `detect_std_flag` prefers
  `-std=c23` and falls back to `-std=c2x`, and gcc-13 is the only compiler in this
  repo's set that takes the fallback — drop it and that branch of the probe is
  never executed by CI again. Each row asserts the flag it is there to exercise
  **and** that the other version's flag is still refused, so the matrix cannot
  quietly degenerate into two identical rows.

  Each row also asserts that its compiler still accepts
  `-Werror=enum-conversion`. That flag is how the enum tier described in
  [09-type-design.md](09-type-design.md) is enforced on this lane, and
  `detect_enum_flags` **silently omits** any spelling the compiler rejects —
  correct behaviour, and also a way for the lane to become advisory with nothing
  going red. `fail-fast` is off, because a failure on one toolchain is a fact
  about that toolchain and says nothing about the other.

### `mcfish_perft.yml` — nightly deep perft

The push lane's perft is capped by a sub-minute budget, and depth is what perft
coverage is made of: en passant exposing a pin, a castling right lost to a rook
capture several plies back, an under-promotion with check. Those live a few plies
below where the fast gate stops. This lane spends the time, against published
reference counts, on the six standard positions. Same rule as above — the counts
are facts, so a mismatch is a movegen bug.

### `mcfish_fuzz.yml` — nightly bounded fuzzing, three jobs

**`fuzz`.** The golden transcripts `golden` diffs against only exercise the
well-formed subset of the UCI grammar. This job drives the ASan+UBSan engine
with seeded pseudo-random command streams — boundary values, truncated and
mangled lines, binary junk, weighted toward almost-valid input, which is where
a parser actually dies — via `tools/uci_fuzz.py`, which owns the contract:
every stream must end with a clean exit and a silent sanitizer, including the
`CRITICAL ERROR` exit-1 path. The first session of this harness found two real
shell defects before it ever ran in CI (a worker-set leak on the
critical-error exit path, and `bench 1 1 2` dying on an empty filename where
the golden searches a nonzero node count).

`Hash` and `Threads` are the two options whose value becomes an allocation, so
they are generated from bounded pools and emitted unmangled — a truncating
mangle rewrites a value `accepts` refuses into one it honours, and the
resulting table is mapped and then touched page by page by the resize clear.
That exhausts the machine rather than the process, so the OOM kill takes the
harness too and CI sees a dead runner with no stream to read. `unbounded_request`
re-checks every payload before it is fed to the engine, so an edit to the pools
fails loudly instead of silently. The pools still reach the refusal paths: the
advertised `Hash` maximum is in range, so it reaches the allocator, but no box
can back it, so the mapping is refused without a page being touched.

**`fuzz-search`.** The in-process complement: `./build.sh fuzz-search 600`
under libFuzzer+ASan+UBSan, real search with no shell and no UCI text in the
way — see the `test` table above and
[00-architecture.md](00-architecture.md#what-the-library-boundary-buys). It
covers movegen, the move picker, the TT, pruning, qsearch and the NNUE
accumulator push/pop, none of which `fuzz` reaches once a mutation is a
well-formed command; neither job subsumes the other.

All three libFuzzer lanes pass `-print_funcs=0`, and it is not cosmetic:
symbolizing each newly-covered function costs seconds per call, and this job had
been executing **three inputs** per 30 seconds of budget — a nightly ten-minute
run that fuzzed nothing while reporting a clean pass.

**An exit code cannot tell "found nothing" from "fuzzed nothing", so the steps no
longer rely on one.** Each lane reads back its own
`stat::number_of_executed_units` and fails below a floor of roughly 1–2% of what
that lane clears locally — low enough that a slow runner passes, high enough that
a stalled one cannot, and a missing count fails too, because it means the run
never reached its summary. Verified in both directions rather than assumed: with
`-print_funcs=0` removed, the search lane reproduces the historical stall at 3
inputs and the step now exits 1. zfish reached the same conclusion from the other
side — its fuzzer prints no total at all, so it decodes the coverage file's
header instead (zfish `b9aa6d03`).

**`fuzz-tb`.** The third input surface, and the only **binary** one besides the
net: a `.rtbw`/`.rtbz` the engine did not write, reached by pointing
`SyzygyPath` at it. Neither of the other two jobs can get there — the UCI
fuzzer mutates text, and `fuzz-search` never loads a table — so the bounds in
`decode.c` and `registry.c` had no fuzzer behind them at all until this job,
which is to say they were a claim rather than a gate. Standing it up found
three bugs, two of them in the decoder that runs inside the search: an
out-of-bounds read through the backward block walk, a btree descent that a
cyclic tree made non-terminating, and a shift of 64. Widening the whole-file
lane past the probe, to the root ranking that indexes with a probe's answer,
found a fourth: an unbounded WDL score reaching `WdlToRank[wdl + 2]` as a
negative index. **A lane that stops at the value does not gate what indexes
with it.** The whole-file lane also counts what it REACHED — rounds, files the
parse accepted, probes that answered — and floors each, because its executed
count cannot tell a decoder walked thousands of times from a parse that refused
every file: the driver supplies the magic, so an input dying on a length check
counts as an execution just the same. Measured against a parse forced to refuse
everything, the executed floor passes at 2,359 inputs and the parsed floor goes
red at 0. `./build.sh fuzz-tb 600`
runs **both** lanes for ten minutes each — see the step table — and the job
fetches the 3-man set first to seed the whole-file one. That fetch is
best-effort: a mirror outage weakens the seed, and weakening a fuzz corpus is
not a reason to fail a build.

All three scheduled daily, 04:31 UTC.

### `mcfish_upstream_check.yml` — weekly upstream-sync detection, three jobs

**`upstream-check`**, two halves. The **gating** half clones the golden at the
pinned SHA and runs `./build.sh upstream-map` — the declared-vs-derived
citation audit (ROT and DRIFT both fail it) plus the uncovered-upstream-surface
ratchet — which the parity workflow cannot run itself because a CI checkout
carries no sibling `../Stockfish` at the pin. The **detection** half is
judgment-free: it counts how many commits upstream `master` is ahead of
[`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE) and
previews the per-owner worklist, so drift cannot silently accumulate between
sessions. Porting stays a deliberate, human-gated session — this job only
detects, it never attempts a port.

**`upstream-nodes`** asks a different question: not whether upstream has
moved, but whether mcfish is still faithful to the pin it already claims to
match. It builds mcfish, clones the golden, builds the pristine oracle at
[`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE) via
[`../tools/upstream/upstream_oracle.sh`](../tools/upstream/upstream_oracle.sh),
then runs `./build.sh upstream-nodes` with a fresh wall-clock seed each time —
the same reason the nightly UCI fuzz job seeds that way, so coverage broadens
over time instead of re-testing the same positions every run.
`net_identity_or_die` (see *Three fidelity probes* above) refuses the
comparison outright, naming which side is wrong, if the net-fetch or
oracle-build steps produced two engines running different nets.

It drives **four position classes**, because a random walk from the start
position reaches exactly one kind of position and an engine branches on many —
so a Chess960 castling divergence, a tablebase-range endgame or a position at
the 50-move boundary was unreachable by this probe however many positions it
drew. The classes come from
[`../tools/fixture_properties.tsv`](../tools/fixture_properties.tsv):

| class | how it is reached | why the walk cannot |
|---|---|---|
| `random` | 12 random legal plies from the start | — |
| `chess960` | a rejection-sampled legal back rank, then the same walk, with `UCI_Chess960` set on **both** engines | the walk only ever starts from the standard array |
| `rule50` | the same walk, then the halfmove clock wound to 90–99 | 12 plies from the start leave the counter near zero every time |
| `endgame` | the FENs in [`../tools/cases/tb.fens`](../tools/cases/tb.fens) | random play does not trade down, and constructing few-man positions here would need a legality model with nothing to check it against |

Each class is reported with its own count, and a class that produced **no**
positions exits 2 rather than being summed into a passing total — an empty class
reads as coverage and compared nothing. `--classes` selects a subset; an unknown
name is refused rather than silently narrowing the run.

Every job in this file runs weekly.
