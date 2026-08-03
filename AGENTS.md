# AGENTS.md

mcfish is a **C23 port of Stockfish**, built with clang by `./build.sh`. The goal
is a **bit-exact 1:1 clone** — same bench signature, NNUE, Syzygy, Lazy-SMP.

Read [docs/](docs/README.md) for the architecture, and
[CONTRIBUTING.md](CONTRIBUTING.md) for the workflow. This file is only what an agent
gets wrong before it has read either.

## The golden

`../Stockfish` is the **golden** — it defines correct behaviour and the differential
gate compares mcfish against a pristine upstream build. Where mcfish and Stockfish
disagree, Stockfish wins.

## The siblings

There are **three** peer ports of the same golden beside this one, and none of them
is a source:

| tree | language | what it is |
|---|---|---|
| `../zfish` | Zig | the tree mcfish was first ported FROM; that relationship is over |
| `../rfish` | Rust | a peer port, swept four times into this tree |
| `../fcfish` | C17 | a peer port written to parse under Frama-C |

Four consequences an agent gets wrong before reading
[tools/upstream/README.md](tools/upstream/README.md):

- **No tree is behind another.** There is no pin for any sibling and `sync-status`
  does not mention them. Most of any of those logs is language work that will never
  have a counterpart, so a "78 commits behind" line would be a false alarm by
  construction. None of them pins mcfish either.
- **Sweeps run BOTH ways, and the log does not tell you which.** The trees
  cross-port constantly and none of them cites the others reliably. In the
  2026-08-01 sweep the numa insert, the `NumaPolicy` parse and the `setoption`
  grammar all turned out to flow mcfish → zfish; the fuzz-lane split and the
  whole-file tablebase harness flowed mcfish → rfish. Read the code, not the
  subjects.
- **A sibling finding is a hypothesis about this tree, not a bug report.** Probe it
  against the oracle before writing a fix: of the eleven behaviours the fourth rfish
  sweep named, seven were already correct here, and the fifth (2026-08-03, rfish's
  NNUE campaign) took **nothing** — the hop-by-hop walk-back, the king-move hybrid
  and the per-perspective refresh set were already upstream's shape here, and its
  remaining wins are Rust bounds-check removal with no C analogue. Most of that
  sweep flowed the OTHER way: rfish's instruction budget, its `native` tier
  selector and its compared-nothing guard each cite a mcfish commit. `git log
  --grep=rfish` and `--grep=zfish` find what each past sweep took and, in the
  bodies, what it probed and left alone.
- **A measurement does not transfer, in any direction.** A win in one language's
  codegen can be flat or negative in another's — zfish's runBack inline won 1.0%
  there and measured FLAT here. Re-measure or do not take it, and search the
  commit log first: a refutation is recorded in the body of the commit that
  refused it, so `git log --grep=zfish`/`--grep=rfish` finds what has already
  been tried and measured negative here.

Only Stockfish is an authority. Where a sibling and Stockfish disagree, that is a bug
report for the sibling.

## Known limitations

Do not document, gate, or optimise around the current shape as if it were the
intended end state. Check the state against the tree before acting on it — the
reliable test is whether a file appears in `build.sh`'s `SOURCES`, because a module
outside it is unwired, not deferred:

- **Syzygy tablebases** — wired. All six `src/platform/syzygy/` files plus
  `tablebase.c` are in `SOURCES` **and** `ENGINE_SOURCES`, the four UCI options
  are live, and `./build.sh tb` gates discovery and the root probe against the
  oracle. `./build.sh tb-fetch` gets the 3-man set into `resources/syzygy/`;
  without it the gate checks
  discovery only and says so. The `d` command prints `Tablebases WDL:`/`DTZ:` lines
  once a `SyzygyPath` covers the position. Cursed-win / blessed-loss is covered by
  `./build.sh tb-cursed`, which needs `./build.sh tb-fetch 5` and is deliberately
  **not** in `parity` — a gate that is usually skipped stops being read — so run it
  by hand when touching the prober.
- **Lazy-SMP threading and NUMA** — **wired.** `Threads` builds a worker set,
  `NumaPolicy` chooses the topology it binds under, and a `go` runs N workers over
  one root. `worker_pool.c` is the driver; every piece of per-worker state that
  was a file-scope static now lives in the `SearchWorker` block, and the pool's
  totals reach the search only through `pool_source.h`, which answers with thread
  0's own values at `Threads 1`. Still open: the NNUE network does not register
  itself for NUMA replication, so a policy change re-partitions the threads without
  re-replicating any weights, and no unit test constructs a `SearchWorker`. See
  [docs/04-multithreading.md](docs/04-multithreading.md).
- **The option model** — wired, and no longer a subset: the `uci` handshake
  advertises exactly upstream's option set, which the `golden` gate pins byte for
  byte, and `engine.c` points the search's seam at the live table
  (`search_set_option_source`), so an option a caller sets is the value the search
  reads. The zero-returning defaults in `search_common.c` are the headless fallback
  for the engine zone linked without a shell, not what the binary runs on.
- **`go nodes N` matches upstream exactly** since two fixes: the time-check
  counter persists across `go` (upstream resets it only in `ThreadPool::clear`),
  and a no-legal-move root still bumps the TT generation (upstream runs
  `tt.new_search()` before its `rootMoves.empty()` check). `bench <tt> 1 <N>
  default nodes` equals the golden's total at every N tested, including the
  full 51-position suite at N=100000.

The bench signature in `tools/signature.golden` is **upstream's number**, and
mcfish currently produces it — matching Stockfish at
`tools/upstream/UPSTREAM_BASE`, which `./build.sh upstream-parity` is what
checks. The sibling's anchor is not evidence about this one — it syncs on its own
schedule and is often at a different commit. It is a bit-exactness anchor,
not a local snapshot; run `./build.sh signature` for the value. A change that moves it is a behaviour change and must say
what moved it.

Bit-exactness is not the same as faithfulness, and the anchor cannot tell them
apart: the bench is a fixed position list, so a divergence that never fires on
those 51 positions is invisible to it. `tools/upstream_nodes.py` is the check that
is not fooled — it drives both engines over positions reached by random legal
moves, which appear in no bench list and no golden.

## Setup

```sh
./build.sh              # binary is `mcfish`, at build/mcfish
./build.sh help         # every step
./build.sh parity       # the aggregate gate — run before calling anything done
```

There is **no Makefile and no build system**. A new `.c` file must be added to
`SOURCES` in `build.sh` — and, if it belongs to `engine/` or `platform/`, to
`ENGINE_SOURCES` too, or `zone-check` and the test binary will not see it.

## Gates

**A behaviour-changing edit is not done until a gate says so.**

```sh
./build.sh parity       # build, zone-check, fmt, docs-lint, test, signature,
                        # simd-scalar, perft, golden, tb
./build.sh signature    # just the anchor
./build.sh test         # unit + property suite, ASan+UBSan
```

`parity` names any gate it skipped for a missing tool. A skipped gate proves
nothing — never report it as a pass.

## Performance work

Read [docs/08-idiomatic-c.md](docs/08-idiomatic-c.md) (porting patterns,
measurement discipline) and [docs/09-tooling-ci.md](docs/09-tooling-ci.md)
(measurement tooling and its blind spots) before proposing any optimisation —
they carry every rule this tree has paid to learn, and each perf commit carries
its measured evidence in its body: **the commit log is the ledger; search it
before re-deriving an idea.** Four rules that outrank intuition here:

- **Establish the effect with `tools/nps_ab.sh` BEFORE opening a counter.** It reads
  the engines' own search clocks, so no startup is in it and nothing has to be
  subtracted. Alternate the order, sum over a position set (bench's 4th argument
  takes a FEN file), and measure the binaries that actually play — tier and PGO change
  the answer, and one deficit here doubled between sse41-plain and icl-PGO.
- **Run `perf_fingerprint.py compare --calls` SECOND.** It is inlining-immune and
  answers "do we run Stockfish's algorithm?". On the spine it comes back exact,
  symbol for symbol, which retires every "we must be doing extra work" theory in
  one command. A call-count divergence is an algorithm bug and outranks every
  cost finding.
- **Subtract startup, by measurement, or the instruction axis will lie to you.**
  `perf_counters.sh` counts the whole process and mcfish parses the net in about
  half upstream's time. The spine instruction ratio reads 0.940 whole-process and
  **1.002** startup-subtracted — the entire apparent lead was the net load, and
  every standing taken before the correction carried it.
  `tools/perf_delta.py` does the subtraction on absolutes, the only place it can
  be done — **for instructions and macro-ops**. For CYCLES the same subtraction
  removes a term whose error rivals the effect, and it reported a 4.3% search
  deficit that direct measurement (bench's own `Total time`, which contains no
  startup by construction) put at 1.004 and 1.018. Time the search, do not
  subtract it.
- **Isolate the component instead of attributing it.** `perft` is the board zone
  alone; `MCFISH_EVAL_MATERIAL=1` is the spine and search with the network gone.
  Comparing the same pair over both localises an effect to a zone in two commands.
  Attribution across two differently-inlined binaries is void by construction.
- **Size an Elo run BEFORE you start it.** Speed converts at ~70 Elo per doubling,
  so a 6% per-node change is ~6 Elo and needs ~10,000 games/cell to see; a
  1000-game cell carries ±18. Two runs of the same binaries at the same TC here
  differed by 21 Elo on the opening seed alone (−18.43 vs +2.78). A cell that
  cannot resolve your effect does not return "no change", it returns a coin flip
  with a sign — and cells that each carry ±18 must never be compared to each other.
  For a few-percent change `tools/nps_ab.sh` is the STRONGER measurement.
- **Gate on the clock, and validate any counter before believing it.** A change
  can be instruction-neutral, cache-better and branch-level and still cost 4% in
  cycles. And a counter opened by name is a hypothesis —
  `tools/perf_counter_validate.c` checks it against two known bottlenecks; two
  conclusions in this tree have died there. The instruction counter is also
  **blind to `rep stosb`** and to software prefetch.

Measure every edit **whole-binary**. The specialized node bodies swing under
register-allocation changes and tt.c micro-edits flip LTO inlining; instruction
arithmetic over a diff is a guess, never a measurement.

## Fleets and subagents

Multi-agent perf fleets are a standing pattern. Each rule below was paid for here
or in zfish's fleet campaigns:

- **Never `git stash`** — the stash is repo-wide across worktrees; pop only a stash
  you created, by index, immediately.
- **Check a gate's EXIT CODE, never a piped fragment** — `./build.sh fmt | tail -1`
  reads exit 0 from tail while the gate is red; this has laundered red gates in
  both ports. Test `$?` of the gate itself, or run it unpiped.
- **`./build.sh build` explicitly before every measurement** — `signature` does not
  always rebuild, and a stale binary has produced false conclusions twice.
- **Charter disjoint FILES, not just disjoint metrics** — two lanes converging on
  the same function from different charters produce conflicting or subsumed
  patches the integrator must untangle.
- **Unique scratch filenames, and verify profile provenance** — concurrent agents
  sharing a scratchpad have clobbered each other's callgrind outputs; check the
  `cmd:` header names your binary and the run carries its `Nodes searched` line
  before trusting any profile.
- **Worktree agents deliver patches, never commits** — and gitignored local note
  directories do not exist inside a worktree: findings travel in the final
  report, and the integrator lands them.
- **A subagent is not re-woken by its own background jobs** — wait on a
  measurement with a foreground `until` loop, or the agent stalls silently until
  someone nudges it.
- A worktree starts where its branch last was, not at your HEAD — reset it to the
  intended base and re-verify with `git log` before building a baseline.

## Traps that cost real time

| trap | where |
|---|---|
| `signature-update` / `golden-update` on a **red** gate launders a bug into the anchor. Fix the code, then re-derive. | [CONTRIBUTING.md](CONTRIBUTING.md) |
| `tools/perft.table` is **not** a golden. Those counts are facts about chess; a mismatch is always a movegen bug. | [CONTRIBUTING.md](CONTRIBUTING.md) |
| "Improving" on upstream. A cleaner formulation that moves a rounding boundary moves the node count. | [docs/08-idiomatic-c.md](docs/08-idiomatic-c.md) |
| Integer semantics differ across C++/C at the edges, and upstream relies on wrapping in places. | [docs/08-idiomatic-c.md](docs/08-idiomatic-c.md) |
| Comments are **imperative mood**; never pin a number a gate computes. | [docs/README.md](docs/README.md) |
| `perf-budget` measures the EXISTING `build/mcfish`. The stamp rebuild and the tier-keyed budget close the old fake-2x-regression trap between tiers; what is left is a rebuild landing inside the timed step, so run `./build.sh build` first anyway. | [docs/09-tooling-ci.md](docs/09-tooling-ci.md) |
| `tools/perf_callgrind.sh` prepends `bench` itself — pass only the bench ARGS, or it profiles a startup-only error run that looks plausible. | [docs/09-tooling-ci.md](docs/09-tooling-ci.md) |

## Commits

**One logical change per commit** — a commit that touches three modules cannot be
bisected when the node count moves.

Conventional subject ≤72 chars, blank line, body wrapped at 80 carrying the
evidence: gate output and exit code, not "should work". **Don't** `git push` —
commit locally and stop unless asked. **Don't** add co-author or generated-by
trailers.
