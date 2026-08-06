# Glossary

The words the rest of this set uses without stopping to define them, in tiers
that must not be confused:

- **Section 1 is Stockfish's vocabulary.** Upstream owns the word; the entry says
  which symbol carries it here. It does not teach the concept — a page in this
  set describes what this codebase does, and the domain reference is a link in
  [11-references.md](11-references.md).
- **Section 2 is this repository's vocabulary.** None of it appears in the
  Stockfish source, and upstream is not obliged to agree with any of it.
- **Section 3 is the words that mean two things here.** Each entry is a
  disambiguation rather than a definition.
- **Section 4 is the testing field's vocabulary.** Neither tree owns it; the
  literature does, which is what makes it worth using. A term there is searchable
  outside this repository, and a step name is not.

A reader who cannot tell which tier a word is in will grep the Stockfish source
for `zone` and not find it.

Audience: all contributors.

Every entry names the file, symbol or step that owns it, and none quotes a number
a gate computes.

**What this page does not cover.** It defines terms; it does not explain
subsystems. For how one search flows, see
[00-architecture.md](00-architecture.md); for what a step proves, see
[10-tooling-ci.md](10-tooling-ci.md); for what a quantity denotes and why so few
of them are types, see [09-type-design.md](09-type-design.md).

## 1. Upstream's word, and what carries it here

Grep the symbol if a citation misses; the owners move faster than the
definitions do.

| term | what carries it here |
|---|---|
| **bench** | the fixed script the anchor is a fact about: `BenchDefaults` in [`../src/shell/bench_positions.c`](../src/shell/bench_positions.c), which is upstream's `Defaults` entry for entry, composed into a session by `benchmark_run`. Changing an entry is a behaviour change that cannot be compared against upstream afterwards |
| **the bench signature** | the node total that run prints, asserted by `./build.sh signature`. The *number* lives in `tools/signature.golden` and in no page |
| **node** | one execution of a node body — `search_node` and its two specializations for alpha-beta, `qsearch_node_pv` and `qsearch_node_nonpv` for quiescence. `ctx_nodes` is the count `go nodes` stops on and the `currmove` threshold reads. **Not** a NUMA node; see Section 3 |
| **`Value`, `Key`** | a score and a Zobrist key, each a plain typedef rather than a distinct type. That is a decision with measurements behind it, not an omission: [09-type-design.md](09-type-design.md) says why a wrapper costs more than it catches |
| **depth** | a plain `int`, for the same reason: a depth-scaled product feeds six different codomains, so a type that carries its unit through one of them breaks the other five |
| **the root, PV, MultiPV** | the root move list is `RootMove` records held in a `RootMoveList`; `search_emit_pv` prints one `info` line per PV line, and `MultiPV` is how many it has |
| **currmove** | `info depth D currmove M currmovenumber N`, the root move now being searched. Emitted from the root node body only past `ID_NODES_LIMIT_OUTPUT` nodes and only by the main thread, which is why no bench and no golden reaches it |
| **the accumulator** | the incremental half of the NNUE evaluation, in [`../src/engine/eval/nnue/nnue_accumulator.h`](../src/engine/eval/nnue/nnue_accumulator.h). Slot `i` of its stack holds the position at ply `i`, and every make/unmake owes it a bracket — [03-engine-eval.md](03-engine-eval.md) owns the invariant |
| **the feature transformer** | the first NNUE layer, stored as one byte blob. Its region offsets are derived twice — `NNUE_FT_*_OFF` where the parse writes, `NNUE_FT_*_OFFSET` where the accessors read — and a static assertion per region is what keeps the two spellings one layout |
| **WDL, DTZ** | the two Syzygy probe results — win/draw/loss, and distance to zeroing. The prober is `src/platform/syzygy/`, and the `d` command prints both once a `SyzygyPath` covers the position |
| **cursed win, blessed loss** | a win or loss whose DTZ exceeds the 50-move counter, so the result is a draw in play. Only a 5-man table reaches them, which is why `./build.sh tb-cursed` is a separate step |
| **Lazy SMP** | the threading model: N workers over one root, sharing the transposition table. Each worker's state is one `SearchWorker` block, the pool is `worker_pool.c`, and thread 0 is the main thread — the one that reports |
| **the transposition table** | `tt.c`, in clusters. `depth8 != 0` **is** the occupancy test, so the `DEPTH_ENTRY_OFFSET` bias `tt_save` applies is load-bearing: an unbiased store at the offset depth is indistinguishable from an empty slot |
| **the history block** | one flat `Histories` object holding the main, capture, continuation and correction tables. A key selects a plane; see Section 3 |

### The port map is the other direction of this table

Section 1 answers "upstream says X, what is it here?". The reverse question —
"upstream's `search.cpp:2088`, where did it land?" — is answered by
[`../tools/upstream_map.tsv`](../tools/upstream_map.tsv) and by the `Golden:`
line each module's header carries.

## 2. This repository's vocabulary

None of these appear in the Stockfish source. Where a script owns the
definition, the script wins.

| term | what it is |
|---|---|
| **zone** | one of the three directories the dependency rule is stated over: `src/engine/` (the chess library), `src/platform/` (the OS runtime), `src/shell/` (the process). `./build.sh zone-check` enforces the direction at **link** time by building engine + platform against a stub `main` |
| **wired, unwired** | in [`../build.sh`](../build.sh)'s `SOURCES` array, or not in it. A `.c` outside both `SOURCES` and `ENGINE_SOURCES` compiles nowhere and is covered by no gate — that is the one structural fact [README.md](README.md) opens with. "Unwired" is a gap being named as a gap, never a design |
| **gate** | a `./build.sh` step that **asserts**, and fails non-zero when the assertion breaks. A step that only builds, measures or re-derives is not one, and is listed with its reason in `LANE_EXCUSED` so `lane-coverage` can tell the two apart. A gate whose tool is missing exits 127 and is skipped, which is not a pass |
| **lane** | one independently driven run. Usually a CI job under [`../.github/workflows`](../.github/workflows) — `lane-coverage` holds every dispatched step to being in a workflow, in `parity`, or excused — and also one target inside a step that drives several, as `fuzz-tb` drives a parse lane and a whole-file lane. A SIMD lane is a different word; see Section 3 |
| **the golden** | `../Stockfish`, the tree that defines correct behaviour. Also a `tools/*.golden` file, which is a different thing entirely — Section 3 |
| **the anchor** | the bench node total, pinned in `tools/signature.golden`. It is a **bit-exactness** claim against upstream at `UPSTREAM_BASE`, not a local snapshot, and a change that moves it must say what moved it |
| **the oracle** | a pristine upstream build, produced by [`../tools/upstream/upstream_oracle.sh`](../tools/upstream/upstream_oracle.sh), that the local differentials drive beside mcfish. **LOCAL**: a CI checkout does not carry one, so every step that needs it says so |
| **the finish line** | `./build.sh upstream-parity` — mcfish's bench against that build. The anchor says the number has not moved; the finish line says the number is upstream's |
| **the port map** | [`../tools/upstream_map.tsv`](../tools/upstream_map.tsv), upstream file to mcfish file, plus the SHA pins in `tools/upstream/`. `./build.sh sync-status` reports the distance to the golden's head |
| **sibling** | `../zfish`, `../rfish` and `../fcfish`: peer ports of the same golden. **None of them is a source and none is behind another**; a finding in one is a hypothesis about this tree, to be probed here before it is fixed here. [AGENTS.md](../AGENTS.md) owns the rules |
| **sweep** | driving one question across a whole class rather than fixing the instance in front of you — every include list, every named file, every sibling commit in a window. A **sibling sweep** is the special case: one sibling's log across a window, each finding probed against this tree before anything is written |
| **seam** | the indirection through which one zone reads a value another zone owns, so the dependency does not run backwards. The search's are `option_source.h`, `pool_source.h`, `time_source.h`, `tb_source.h` and `output_sink.h`; the shell installs them before every `go` (`install_seams`), and each answers with a headless default when nothing does, which is what keeps the engine zone linkable alone. The perf pages use the word for a second thing; see Section 3 |
| **the spine** | the engine with the network removed — the board, state and search machinery an `MCFISH_EVAL_MATERIAL` build leaves running. A **spine comparison** is that pair of builds measured against the oracle patched with the same formula: it localises an effect to a zone by removing a component rather than attributing one |
| **tier** | an ISA target the build selects into the binary, chosen by `MCFISH_ARCH`. A measurement is a fact about **one** tier, and `native` names a different tier on every host, so a result carries the tier it was taken at |
| **knob** | an environment variable that builds a **deliberately different** binary for measurement — `MCFISH_EVAL_MATERIAL`, `MCFISH_SIMD_SCALAR`, `MCFISH_ACC_STATS`. Not UCI options, and not shipped behaviour: a knob build plays badly or reports extra on purpose |
| **the budget** | `./build.sh perf-budget`, which holds the retired-instruction count to a per-tier recorded row. It **subtracts startup**, so it is structurally blind to the net load and the magic tables — Section 3 for the other budget |
| **fixture, property, witness** | the three columns of [`../tools/fixture_properties.tsv`](../tools/fixture_properties.tsv): the behaviour the engine branches on, the file that presents it, and a regex that must still match inside that file. `fixture-coverage` holds the table in both directions, so a case file under no property is a failure |
| **transcript case** | a `.uci` script under [`../tools/cases/transcript`](../tools/cases/transcript) driven through both engines by `upstream-transcript`, whole output diffed. A `# hold <seconds>` line — a comment to both engines — makes the wait a deadline for a case whose search needs one |
| **rig fault** | a verdict that the comparison **did not happen**: both sides blank, a case that never answered, an empty corpus. It is neither a pass nor a failure, and it is reported before any standing, because a run that compared nothing must not publish the standing of what it did compare |
| **known divergence** | an argued regex in [`../tools/transcript_known.txt`](../tools/transcript_known.txt), tagged `EXPIRING` or `PERMANENT`. An `EXPIRING` entry that matches nothing in a run fails the gate: a filter that outlives its cause is how a differential quietly stops comparing |
| **fact table** | a file of facts about chess rather than about mcfish — [`../tools/perft.table`](../tools/perft.table) is the one. It is **not** a golden and is never re-derived from this engine; a mismatch is always a movegen bug |
| **oracle-derived, self-golden** | which engine a golden was driven from. `golden-audit --write` and `tb-update` drive the **oracle**, so what they write is upstream's bytes; `golden-update` drives **mcfish**, so what it writes is a photograph that pins a defect exactly as faithfully as correct behaviour. [`../tools/GOLDEN_PROVENANCE.md`](../tools/GOLDEN_PROVENANCE.md) owns the distinction |
| **ratchet** | a recorded set a gate allows to move in one direction only, failing in both: `engine-standalone` holds the engine→platform undefined symbols to `tools/engine_platform.baseline`, and `upstream-map` holds the uncovered upstream surface to `tools/upstream_map.baseline`. A NEW entry is a fresh dependency; a vanished one asks to be deleted, because a baseline that only grows stale stops describing anything |
| **the ledger** | the commit log, read as the record of what has already been measured. Every perf commit carries its evidence in its body, including the refutations, so `git log --grep` finds an idea that has already been tried and measured negative here |
| **fleet** | several agents measuring in parallel, chartered onto **disjoint files** rather than disjoint metrics, delivering patches rather than commits |
| **quiet box, the A/A floor** | an idle machine, and the noise floor obtained by A/B-ing a binary against a byte-identical copy of itself. Each axis has its own floor, and a change smaller than the floor of the axis you measured is not a result |

## 3. Words that mean two things

| word | meaning A | meaning B |
|---|---|---|
| **golden** | `../Stockfish`: the tree that defines correct behaviour. Also *a* golden — the upstream file or symbol one module was ported from, which is what the `Golden:` line in a header names | a `tools/*.golden` file: a pinned transcript of what **mcfish** printed. Nothing makes the two agree — `golden-audit` re-derives from the oracle, `golden-update` from this engine |
| **oracle** | the pristine upstream build the differentials drive | the testing-field term in Section 4: whatever decides that a result is correct |
| **node** | one search node body | one NUMA node: a set of CPUs in a `NumaConfig`, which `numa_config_add_cpu_to_node` fills |
| **lane** | one CI job | one SIMD lane, in `simd.h`'s vectors and the per-lane C expression the scalar body spells out |
| **source** | an entry in the `SOURCES` array, so a file that is in the binary | a `_source.h` seam, so a value the engine zone reads through an indirection. And "the source" a port was made **from** — which no sibling is |
| **sweep** | a class swept across the tree, or a sibling's log across a window | a gate's own pass over its inputs — the transcript loop over its cases, the whole-file tablebase lane over what it synthesized |
| **budget** | the per-tier instruction budget `perf-budget` holds | the search's own time budget, which the time manager resolves once per `go` |
| **key** | a Zobrist `Key` off the position | the material key a Syzygy probe hashes its table by, and the pawn and correction keys that select a history plane. Same word, three spaces, no shared arithmetic |
| **stack** | the search's per-ply `Stack` array, the `ss` every node body indexes | the NNUE accumulator stack, one slot per ply, pushed and popped by the make/unmake bracket |
| **worker** | one `SearchWorker` block: the per-thread search state | the OS thread the pool spawns and NUMA binds. One block per thread, and neither word implies the other |
| **corpus** | a fuzz seed corpus under `build/`, which `assert_fuzz_executed` holds to having actually been driven | `docs_lint.sh`'s symbol corpus: every tracked source plus the tracked path list |
| **seam** | a source seam: the indirection a zone reads another zone's value through | an inlining seam: a translation-unit boundary the optimiser does not cross, which is the sense the perf pages use for a hot symbol upstream inlines and this build does not |
| **bench** | the UCI command | the position table it runs, and the node total that run produces. "The bench moved" is ambiguous between all three; say which |

## 4. The testing field's vocabulary

No file in either tree defines these. They are worth learning as names rather
than descriptions: each is the handle for a known failure mode, and the last two
describe checks that are worse than absent.

| term | what it means | what it is here |
|---|---|---|
| **oracle** | whatever decides that an observed result is correct | the pristine upstream build, `tools/perft.table`, a sanitizer report, or nothing at all |
| **differential testing** | drive two implementations with one input and diff | `upstream-parity`, `upstream-transcript`, `golden-audit`, and [`../tools/upstream_nodes.py`](../tools/upstream_nodes.py) over random legal positions |
| **characterization test** | pins *current* behaviour, and is explicitly not a correctness claim | every `tools/*.golden`, which is why one re-derived from mcfish rather than from the oracle proves only that mcfish still agrees with itself |
| **metamorphic relation** | a property relating two runs, rather than a pinned value | `net-roundtrip`: write the resident net out, and require the bytes back. `simd-scalar`: two implementations of the same kernel, one anchor |
| **implicit oracle** | needs no reference, because some outcomes are wrong on their face | ASan and UBSan on the `test` step, and the fuzz steps, where the finding is the crash |
| **mutation testing** | inject the defect, and require the check to go red | `negative-control` is the automated form, one mutant per gate; the "seen to fail" table in a gate's commit body is the by-hand one, and [09-type-design.md](09-type-design.md) states it as a step of adding a type |
| **lost test** | a test that exists and is in no suite the build runs | a `.c` in neither `SOURCES` array, and a `.uci` case in no property row. Both have their own gate here for exactly this reason |
| **false pass** | a run that passed because it compared **less**, not because more was right | the empty-corpus, blank-side and rig guards. A gate that reports OK over nothing reads exactly like one that checked everything |
| **negative control** | a run against the **defective** tree that must show the defect, proving the check can fail at all | `./build.sh negative-control`, which mutates the engine once per gate, requires that gate to exit non-zero, then restores and requires it to pass. A gate that has never fired is not a gate |
