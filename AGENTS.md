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

There are **four** peer ports of the same golden beside this one, and none of them
is a source:

| tree | language | what it is |
|---|---|---|
| `../zfish` | Zig | the tree mcfish was first ported FROM; that relationship is over |
| `../rfish` | Rust | a peer port, swept four times into this tree |
| `../fcfish` | C17 | a peer port written to parse under Frama-C |
| `../Stockfish` branch `refish` | C++ | a refactor of the golden IN the golden's own checkout, on the same pin this tree tracks |

**`refish` is the one that does not look like a sibling.** It lives in
`../Stockfish` -- the golden's checkout -- as a branch, so `git log` there answers
for it unless you say `upstream/master`. It is NOT the golden: `origin` is a fork,
and `refish` carries 385 commits on top of upstream. Everything below applies to it
exactly as to the other three, and its untracked dev notes carry a register of
**thirty-four confirmed defects in upstream Stockfish**, each with a reproducer that
was run. All thirty-four are closed or refuted in this tree as of 2026-08-18: the
first twenty on 2026-08-15, and the second campaign's fourteen on 2026-08-18.

The consequences an agent gets wrong before reading
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
- **A whole class can be closed here and still hide one hole.** The sixth sweep
  (2026-08-05) probed both siblings' corrupt-table hardening — four bounds across
  `zfish 741f8ffc/3883af90/6a5596de` and `rfish 6b252a1`. Three were already
  bounded here (the empty lead-pawn collection, the group walk past `size`, and
  the range-checked geometry accessors in `encode.h`), and the fourth was a live
  out-of-bounds — but **not where either sibling hit it**. Both siblings trap
  inside the tablebase zone, which is bounded here; the unbounded consumer in this
  tree is one zone over, in the search's root ranking, and no sibling names it.
  Take the CLASS as the hypothesis, then find this tree's own consumers of it.
  rfish's 2026-08-05 NNUE work again took nothing: its tiered fold tile cites
  mcfish's `ROW_TILE_WIDTH` as the source, and its constant-trip-count win is Rust
  slices carrying no length past a borrow — C arrays already carry their extent.
- **A sibling's GATE is a hypothesis about what this tree does not instrument.**
  The seventh sweep (2026-08-06) took rfish's `a469772`, which gates that its net
  survives `export_net`, and asked the same question here. The answer was larger
  than the question: mcfish had no writer at all — no `export_net`, no
  `Network::save`, no LEB128 encoder — and pulling that thread found that the UCI
  dispatch had never been diffed against upstream's arm for arm. Four commands
  were missing or wrong (`export_net`, `help`/`license`, `#` comment lines, and
  the unknown-command reply quoting the token instead of the line), each visible
  in one `printf` and none of them caught by any gate. `speedtest` — which no
  sweep found, because rfish does not have it either — was the fifth, and the
  dispatch now answers all twenty of upstream's commands. When a sibling builds
  an instrument, check what this tree points at that surface — not only whether
  the same defect is here.
- **A sibling's BUG can be this tree's ungated correctness.** The eighth sweep
  (2026-08-06, rfish `a469772..2f1ce01`) took no code: `2f1ce01` is `speedtest`,
  which mcfish shipped first in `cf1178f4`, and `2596bc6` fixes a root `currmove`
  announcement rfish had never wired up — where this tree has all three of
  upstream's call sites and one owner for the threshold. But rfish's line about
  it holds here word for word: nothing prints past ten million nodes under any
  gate, so deleting the call left every one of them green. Ask "would a gate here
  have caught the sibling's bug?" even when the answer to "is the bug here?" is
  no. It is now adjudicated (22 lines, identical to the oracle) and gated, in
  `b83ad32b`.
- **A capability the sibling built a MODULE for can already be a function here.**
  The ninth sweep (2026-08-09) probed zfish's headless engine — its `headless.zig`
  build root, its headless lint and its single-position search module — and took
  no code. Its
  structural half is `zone-check` plus `engine-standalone` here, and this tree's
  is stronger: a linker ratchet at an empty baseline, not a text-parsed import
  graph. Its headless-search module exists because zfish had no way to search
  one position without the platform thread orchestrator; here that entry is
  `search_go`, which is what the unit tests and both fuzzers already call. What
  transferred was the sibling's REASON: its lint asserts `headless.zig` imports
  every engine module because a hand-maintained proof root cannot report that it
  got smaller — and `zone-check` proved exactly what `ENGINE_SOURCES` listed, with
  a shell file smuggled into that array making it pass (verified against HEAD:
  `zone check passed`, exit 0). Both are gated in `a434c588`. Its always-run
  search-from-a-reached-position test transferred the same way: the class, not the
  bug — the root-setup defect zfish's version caught cannot occur here, but the
  suite's searches all ran before the net loaded, so **nothing in it had ever
  entered the accumulator from inside a search** (`shared 0 split 0 refresh 0`
  before the new test). Closed in `f12320b3`.
- **A curated fixture cannot hold the case nobody thought of.** The tenth sweep
  (2026-08-09, rfish) probed the same headless class and again took no code — the
  crate graph makes rfish's zone check a compile error rather than a gate, its
  engine-side search harness is `search_go` here, and its unenforced intra-crate
  module direction has no counterpart because this tree declares no layering
  rule inside `engine/` and says why. What transferred was three randomised
  shapes its harness runs and this tree's always-run suite had only in curated
  form: unwinding a 60-ply line (`5f50ac3e` — a castling-key toggle skipped once
  the halfmove clock reaches 4 is invisible to every other gate, including the
  depth-3 exhaustive round-trip), random and mutated FEN strings (`c54301a5` —
  dropping the placement parser's file bound is a UBSan out-of-bounds this walk
  raises and the reject table cannot, because the curated overflow lands INSIDE
  the board array), and the zero-wait `stop`/`quit` (`37070852` — every existing
  async invariant sleeps first, so none of them drove the shape every harness
  sends). Ask what a sibling's harness EXPLORES that this tree only enumerates.
- **A defect you DECLINE to port needs the same evidence as one you take.** The
  eleventh sweep (2026-08-15, the first of `refish`) closed the last two of the
  twenty upstream defects still open here, and both had been deferred earlier in
  the same session on reasoning that did not survive being checked. `nodestime`
  making `movetime` mean nodes was declined as "a defined-behaviour divergence the
  bench and goldens depend on" — a claim carried over from a sibling where it was
  true; here `nodestime` appears in `tools/` exactly once, as the handshake option
  line, so the conversion is inert in every gated path (`03c6aeae`). `hash_bytes`'
  sign-extended tail was declined as "reproduced on purpose, and its consumers are
  dead" — and dead is precisely why it was safe, since the net reader compares the
  u32 ARCHITECTURE hashes and never reaches that function (`c7d6749a`). A decision
  to leave a defect alone is a claim about this tree, and it rots the same way a
  port does.
- **A gate ported from a sibling reports on THIS tree's instruments first.** The
  same sweep took `refish`'s corrupt-table fixtures, and the first run found the
  reader refusing a crafted header without saying which file (`c6caae18`) — its
  judge asks four things of a refusal and this tree answered three. Then two of the
  seven fixtures turned out to gate nothing here, because they PASS this parser by
  design and were scored as refusals by a second per-side header landing in zero
  padding (`b079dcaa`). Re-derive what a borrowed fixture actually trips, against
  an instrumented build; a green gate over a hollow fixture is the failure mode a
  sibling cannot warn you about, because in ITS tree the fixture is real.

- **A sibling's OWN numbers already tell you which half transfers.** The twelfth
  sweep (2026-08-18, `refish`'s second upstream campaign, defects 21-34) landed six
  fixes and four perf commits, and every perf decision was made by reading the
  sibling's per-COMPILER columns before writing any code. This tree is clang: the
  pairing-tree repack reads gcc -0.79% and clang -0.15%, so it was refused rather
  than measured — a structural rewrite of a hostile-input parser is not worth a
  clang tenth. `50db0afd`'s arena zeroing and `2faa286a`'s accumulator were already
  this tree's shape, and `2faa286a`'s remaining half is worth clang -0.0007% by its
  own note. What DID transfer transferred larger than advertised, because the
  corpus differs: the bucket-table length lookup measured **-7.09%** here against
  refish's -4.78%, and resuming the escape walk **-4.00%** against its -0.66%/-1.72%
  — this tree's probing lane opens 5-man tables whose `max_sym_len` reaches 20
  against a twelve-bit cap, so the walk it deletes was doing more. Read the columns,
  then re-measure what survives them.
- **The instruction axis and the clock can disagree, and only one tool settles it.**
  The same sweep's `2e3dd920` port reads a clean -0.412% on `perf-budget` with a
  0.00001% floor, while a hand-rolled timing loop called it a 1.3% REGRESSION on
  three consecutive pairs. `tools/nps_ab.sh`, which pins a core and alternates the
  order, put the median at 1.0077-1.0087 in the change's favour over 9 and 17 rounds
  and printed that the spread straddles 1.000 — this host cannot resolve under 1% on
  the clock. Never take a timing number from a loop you wrote yourself. And the
  refutation half is the same instrument: `daa06206`'s `save` hoist measured
  **+0.018%** here, 5.0M instructions above the floor, and was reverted.

- **`refish` REBASES, so a SHA is not a citation.** The thirteenth sweep
  (2026-08-23) opened by checking the SHAs earlier sweeps had cited and found
  many of them off-branch: `git cat-file -e` still resolves them, because a
  rebase leaves its pre-rebase commits in the object store, but
  `git merge-base --is-ancestor <sha> refish` says no. Derive the unswept range
  by CONTENT — find the sibling commit whose change this tree already carries and
  range from there — and cite by SUBJECT, which survives an amend. Two facts make
  this cheap: `cite-check` reads docs pages only, so a rotted SHA in a commit
  BODY is silent, and refish's base is now `229f6339e`, the same pin this tree
  tracks, so its whole log is comparable rather than a moving target.
- **Read the sibling's own columns before writing a line, then re-measure the
  SHAPE rather than the size.** That same sweep took five perf commits worth
  **−0.667%** on `perf-budget`, every one bit-exact against `./build.sh
  signature`, and refused thirteen. Five went on the sibling's OWN numbers: the
  sparse-affine pin (clang −0.0000%), the all-node reciprocal (clang PGO
  +0.0003%), the fail-high divisor (+0.0019% Ir) and the lmr-divisor reciprocal
  (+0.0636% on the lane closest to this one) each read NEUTRAL OR WORSE on the
  clang instruction axis this tree gates on, and their cycle claims cannot be
  settled on this host; the root-window reciprocal quotes no budget figure at all,
  only a codegen census, and after this tree's own window-term hoist its premise
  — six divides in the hot path — no longer describes this tree.
  Six went because the site is already this tree's shape or is absent: the
  psq-before-threat ordering (both updaters already build psq first), the two
  pair-activation clamps (mcfish has no `USE_PAIR_ACTIVATIONS` at any tier), the
  eval-bucket shift (`piece_count_of` already returns `size_t`, so the division is
  unsigned), the ASCII option fold (`ascii_lower` has been here all along), and
  installing the workers' root from the calling thread (`search_go_start` already
  loops over the workers on the UCI thread).
  Two went because no instrument here can price them: the vectorised quiet-sort
  limit test and the output layer's split dot product both claim LATENCY or
  PREDICTION and cost instructions to buy it, and the sampled columns on this host
  have a floor wider than either effect.
  The threat-index fold is HALF here and the half present is stronger:
  `ThreatIndexBlock` already merges `Offsets` into a u16 `comb` plane AND
  colocates `lut1` behind one base, where the sibling's merge leaves `lut1`
  separate. What it has that this tree does not is the dedup of twelve attacker
  rows down to seven — a separate hypothesis about cache footprint, not measured.
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

  Two more instruments own this reader, and both exist because the bench list
  cannot reach it: every bench position has more men on it than any table
  `tb-fetch` installs, so `registry.c`, `do_probe_table` and the decode loop are
  absent from `signature`, `perf-budget` and every counter this tree records.
  `./build.sh malformed` (in `parity`, 2.4 s) drives crafted headers and mutated
  real tables past the sanitized binary and judges refusal and survival — without
  the corpus it NARROWS to the half that needs no tables and says so, the way `tb`
  does, rather than failing;
  `./build.sh perf-budget-tb` measures a PROBING workload against its own
  `<tier>+syzygy` budget row. **Run `perf-budget-tb` on any edit to
  `src/platform/syzygy/`** — it caught a 0.13% regression in a diagnostic that
  `perf-budget` read as free (`647965a6`).
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

## Working here

The rest of this file is about the code. This section is about you.

**Deliver what was asked, at the scope intended.** Make the routine calls yourself
and check in only where two readings of the request would produce materially
different work. If the ask looks mistaken, say so in a sentence and build it anyway
under a stated assumption — quietly narrowing, widening or transforming it is the
failure mode. Finish the whole task; if one part is blocked, finish every other part
and say plainly which one you left and why. Scaling the work down is the user's call.

**The gates ARE the verification — do not invent a second one.** A behaviour-changing
edit runs `./build.sh parity`, an ISA-gated edit runs `./build.sh arch-determinism`,
a thread edit runs `./build.sh tsan`: those are not optional, and the exit code is
the only evidence anyone reads. Re-running a gate that is already green, bolting a
"final check" pass onto a finished task, or having something review your own diff
proves nothing the gate did not.

**Delegate only what is genuinely parallel and large.** A wide multi-file
investigation, or a perf fleet with disjoint charters, earns subagents; work you can
finish in a handful of tool calls does not, and nothing earns a subagent whose job is
to check your work. If one agent can do it, use one. Past two, the fleet rules below
bind.

**Lead with the outcome.** One sentence before the first tool call saying what you
are about to do, then quiet until something changes the plan, then a first sentence
that answers what happened — the node count, the exit code, the ratio — with the
detail after it for whoever wants it. The full evidence goes in the commit body,
which is the ledger a fresh clone gets; the reply is the summary of it.

**Correct only what changes a decision.** If an earlier statement would send a reader
to the wrong file or the wrong number, fix it in a sentence and carry on. For a slip
that changes nothing, fix it and say nothing — a running tally of your own mistakes
buries the correction that mattered.

**Match a document's length to what it must carry**, whether it is a page in
[docs/](docs/README.md) or a report in the reply. Cover the substance and stop: no
restated summary, no recap of what a gate prints, no next-steps list nobody asked
for. Length is not thoroughness; it is where rot hides
([docs/13-writing.md](docs/13-writing.md)).

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
./build.sh parity       # the aggregate: twenty gates. build, zone-check, fmt,
                        # docs-lint, shellcheck, cite-check, type-check, the four
                        # coverage gates, test, signature, net-roundtrip,
                        # speedtest-check, simd-scalar, perft, golden, tb, malformed
./build.sh signature    # just the anchor
./build.sh test         # unit + property suite, ASan+UBSan
```

`parity` names any gate it skipped for a missing tool. A skipped gate proves
nothing — never report it as a pass.

## Performance work

Read [docs/08-idiomatic-c.md](docs/08-idiomatic-c.md) (porting patterns,
measurement discipline) and [docs/11-performance.md](docs/11-performance.md)
(the instruments and their blind spots) before proposing any optimisation —
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
- **Isolate the component instead of attributing it.** `./build.sh perf-decomp <base>
  <head>` attributes DIRECTLY -- callgrind over both binaries, SELF cost per symbol,
  grouped by `tools/perf_components.tsv`, deterministic to the instruction. It
  localised the low-ply hoist to `movepick 0.9417` with every other row at 1.0000.
  Where it cannot reach, `perft` is the board zone alone and `MCFISH_EVAL_MATERIAL=1`
  is the spine and search with the network gone; comparing the same pair over both
  localises an effect to a zone in two commands. Attribution BY HAND across two
  differently-inlined binaries is void by construction -- that is what the tool exists
  to replace. Read its cache columns as a model that RANKS locality, never as time.
- **Measure the threads too, and only with `tools/nps_threads.sh`.** Every other axis
  here runs one thread. That tool is the only one that does not, and it works because
  the node count is its INPUT: a threaded fixed-DEPTH bench is not reproducible even
  against itself -- three runs of `bench 128 8 10` read 3.77 M, 6.14 M and 4.46 M
  nodes, a 62.8% spread, against 0.05% for the same runs under a node budget. Read
  `r(T)/r(1)`, never the A/B column: the latter carries the single-thread speed
  difference inside it, so a binary that is merely faster looks like it scales better.
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

- **Charter a fleet only above the bar in *Working here*** — independent, sizeable
  tracks. Below it one agent working end to end beats three coordinating, and a fleet
  spawned to double-check a finished change buys nothing.
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
| `perf-budget` measures the EXISTING `build/mcfish`. The stamp rebuild and the tier-keyed budget close the old fake-2x-regression trap between tiers; what is left is a rebuild landing inside the timed step, so run `./build.sh build` first anyway. | [docs/11-performance.md](docs/11-performance.md) |
| `tools/perf_callgrind.sh` prepends `bench` itself — pass only the bench ARGS, or it profiles a startup-only error run that looks plausible. | [docs/11-performance.md](docs/11-performance.md) |

## Commits

**One logical change per commit** — a commit that touches three modules cannot be
bisected when the node count moves.

Conventional subject ≤72 chars, blank line, body wrapped at 80 carrying the
evidence: gate output and exit code, not "should work". **Don't** `git push` —
commit locally and stop unless asked. **Don't** add co-author or generated-by
trailers.

## Before you reply

Keep it short and lead with the outcome: what moved, what the gate said, what is
left. The long form belongs in the commit body.
