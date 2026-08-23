# Performance measurement

The nine local-only instruments, what each one proves and what it cannot see, the
order to reach for them in, the two corrections without which the instruction axis
lies, and where this port stands against the golden on the spine.

Audience: anyone measuring a refactor or an optimisation. Every gate that decides
a VALUE is [10-tooling-ci.md](10-tooling-ci.md)'s; this page is the axis that
decides a COST, which no gate here blocks a merge on.

## The instruments, and the order to reach for them

Nine tools in `tools/` that are **not** `./build.sh` steps and **not** gates.
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
| [`../tools/perf_callgrind_delta.py`](../tools/perf_callgrind_delta.py) | startup-subtracted **cache and branch** table from four `perf_callgrind.sh` profiles. Deterministic, so it is the one instrument that can attribute an IPC gap on a loaded box |
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
   for the spine, `MCFISH_ACC_REFRESH_ONLY=1` (optionally with
   `MCFISH_NO_THREAT_RECORD=1`) to price the accumulator's incremental path against
   the rebuild it replaces. Which component owns it. The last two are bit-exact, so
   the node total must not move between the variants — and the same workload check
   is what catches an ablation that quietly searches a different tree.
4. **`perf_counters` + `perf_delta.py`** — only now, and only for the WORK axes.

Going straight to the counters is how a session spends a day attributing a deficit
that the first tool would have measured in five minutes — and, worse, how it reports
parity while a 13% gap sits in front of it.

`perf_counters.sh` drives both binaries interleaved over hardware counters
(`perf_event_open`, so the absent `perf` CLI does not matter), pinned to one core,
and reports the **median of per-round paired ratios**. It is the only tool here
that reads instructions at avx2 and at the AVX-512 tiers — where callgrind SIGILLs on
the EVEX prefix — and the only one that can *see* an IPC gap rather than infer one.

**The workload is a precondition, and it is enforced on every round, not just the
first.** Both modes parse `Nodes searched` out of each run and refuse to report if
it moves: a count is a statement about an amount of work, so an engine that dies
mid-round, a net that goes missing after round one, or an ablation that quietly
searches a different tree would otherwise produce a plausible smaller number that
the budget gate compares as though nothing had changed. `../zfish f876cb5b` credits
exactly this check with catching an ablation searching 162,860 nodes while claiming
163,081 — the instruction delta read clean either way. The harness is also built
under the engine's own warning set with `-Werror`: the binary every perf claim in
this tree rests on should not be the least-checked one in it.

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
- `perf-budget` measures the **existing** `build/mcfish`. The content-hash rebuild
  below now catches a stale `MCFISH_ARCH` automatically — `CFLAGS_ARCH` is part of
  the hashed command, so switching tiers is a stamp mismatch and triggers a
  rebuild before the measurement runs, closing the fake-regression trap this
  bullet used to warn about by hand. What it does not save you from is a rebuild
  landing *inside* the timed step and leaving the machine hot — run
  `./build.sh build` deliberately beforehand for that reason alone.
- **A budget only covers the workload it runs, and this one has a hole with a
  name.** Every bench position has more men on it than any table `tb-fetch`
  installs, so `registry.c`, `do_probe_table` and the decode loop are absent from
  `perf-budget`'s figure entirely — a bound placed inside the decoder is free
  according to it, which is not the same as being free. `perf-budget-tb` measures a
  probing workload against its own `<tier>+syzygy` row, and it is not a hypothetical
  gap: the first change to land after it went in cost **+7,359,682 instructions,
  +0.13%**, on the probing workload while `perf-budget` stayed green, because a
  4200-byte buffer added for a refusal diagnostic sat in a frame whose hot path is
  the early return taken on every probe. Bisect a red row by reverting one file and
  re-measuring — that named the file without guessing.
- **The budget row is keyed by the tier in the binary, and the tolerance was set by
  mutation.** `native` names a different ISA on every host, so a row filed under
  that word is a number about one machine that the next one compares its binary
  against; `MCFISH_ARCH_STRING` is the resolved tier (`x86-64-avx512icl` here), and a
  host whose tier has no row SKIPS at 127 rather than measuring against a stranger.
  This works because **`native` selects an enumerated tier rather than emitting
  host-specific code** — see *The arch ladder* below. Were it `-march=native`, the
  tier name would not describe the binary and the key would be a promise the file
  could not keep.
  The 0.05% ceiling is ~2000x the measured spread of this bench — five runs span
  under 0.00002% — and it is that tight because the previous 0.5% let a real one
  through: forcing `pos_adjust_key50_of` out of line costs +0.238% with `signature`
  green, which is precisely the class this gate exists for. Re-set it the same way
  if it ever moves: a tolerance chosen by feel is a decoration.
- Gates rebuild when the binary's stamp — a content hash of every source, header,
  the full compile command and the compiler's own version — no longer matches
  `$BIN.stamp`, not when the binary is merely older than a file. A touched-but-
  unchanged header, or a clock skew, used to cause both a false-stale rebuild and a
  false-fresh skip under a timestamp comparison; the hash has neither failure
  mode.
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

### What each column can resolve, and the control that says so

Two properties separate the columns, and they decide which may carry a claim.
Retired instructions are **deterministic** here: `perf-budget` reproduces a bench
figure to eight significant digits across independently built binaries, and
`perf-decomp` is callgrind, which is a simulation. Every other column —
cycles, IPC, cache misses, branch misses — is a hardware counter sampling a
shared machine, and none of them may carry a verdict until it has cleared a
control taken in the same session.

**Run the A/A.** Copy the base binary and measure it against itself through the
same harness. Eight paired rounds of `bench 16 1 11` on this host, base against a
byte-identical copy:

| column | A/A control | reads as |
| --- | --- | --- |
| instructions | 1.000 | deterministic — any deviation is a real effect |
| macro-ops | 1.000 | same |
| branch misses | 0.993 | ±0.7% floor |
| cache misses | 0.984 | ±1.6% floor |
| cycles | 0.983 | ±1.7% floor |
| IPC | 1.017 | ±1.7% floor |

A sampled reading inside its own floor is not a small effect, it is *no
measurement*. One commit of the 2026-08-23 refish sweep read cycles 1.010,
branch misses 1.001 and cache misses 1.011 against that control: every column
inside the floor, so the commit resolved on instructions and nowhere else. The
same five commits taken TOGETHER read cycles 1.060, branch misses 1.024 and
cache misses 1.051 — each clearing its floor by three times or more, which is
what an effect looks like when it is there.

**The `perf` CLI is not installed on this host, and that does not matter.**
`perf_counters.sh` opens the counters through `perf_event_open` directly, so
every hardware column above is available. Concluding "no `perf`, therefore no
branch counter" is wrong, and it cost this tree one commit body that had to be
rewritten.

**callgrind's branch simulator is a model, and it can read against a real
change.** `perf-decomp`'s `Bcm` column put movepick at 1.1024 — a 10% mispredict
*regression* — for the commit that took three stages out of the dispatch table,
which is a change aimed squarely at prediction. Hardware read 1.001 on the same
pair, inside a 0.993 control: at parity, not worse. The simulator models a crude
BTB against this part's ITTAGE, and `tools/perf_decomp.sh` says in its own header
that its figures rank locality and do not predict time. Read `Bcm` to find out
*where* prediction moved once hardware has said *that* it moved; never the other
way round.

**Ratios chain, and this tree has checked both halves.** Measured step by step,
each against the binary immediately before it, the five commits of that sweep
read −0.0627%, −0.4453%, −0.0829%, −0.0393% and −0.0379%; their instruction
deltas sum to exactly the direct reading of the first binary against the last,
−174,894,277 instructions, −0.6668%. That closes because each step was measured
against its own predecessor.

Measured against a **common** base it neither closes nor even applies. Each of
the five was cherry-picked alone onto the pre-sweep commit and measured there:

| change | vs common base | vs its predecessor | disagreement |
| --- | --- | --- | --- |
| three list walks out of the dispatch table | −16,411,919 | −16,442,625 | 0.19% of the effect |
| flatten the quiet score's per-move work | −115,464,612 | −116,704,572 | 1.06% |
| reductions table as u16 | −10,237,214 | −10,247,198 | 0.10% |
| stop the good-quiet walk | *does not apply* | −21,619,879 | — |
| carry the reduction window term | *does not apply* | −9,880,003 | — |

Two of the five **cannot be measured against the common base at all**: the
good-quiet stop needs the label hoist the first commit introduces, and the
window-term hoist needs the u16 retype. A fleet chartered on one shared base
cannot produce those two numbers, and the three it can produce are each a
different number from the one the assembled stack pays — same sign and same
order, but off by up to 1% of the effect, because a deterministic ratio is a
fact about the code **and its base together**.

So the assembled figure is a separate measurement, not a product of the parts,
and it belongs to whoever assembles them.

### The bench understates a warm search, and by how much

`perf-budget` runs `bench 16 1 13`: a cold search of 51 fixed positions against an
empty table — and the same 51 the PGO profile is trained on. Anything whose cost
scales with how often a node re-enters a stage, re-scores a list or re-walks a
history plane is entered fewer times per node here than in a real game, so the
gate reads a *lower bound* on the effect.

The 2026-08-23 refish sweep measured that gap directly. Each row is one change,
the sibling's figure taken on a warm 60-ply replay at depth 20, this tree's on the
cold bench:

| change | refish, warm depth 20 | here, cold bench |
| --- | --- | --- |
| three list walks out of the dispatch table | −0.688% (clang -O3) | −0.0627% |
| flatten the quiet score's per-move work | −0.785% (clang -O3, its own budget) | −0.4453% |
| stop the good-quiet walk at the first miss | −0.202% | −0.0829% |
| reductions table as u16 | −0.0477% (its own budget) | −0.0393% |
| carry the reduction window term | −0.0496% (clang -O3, its own budget) | −0.0379% |

The two rows the sibling measured on its own *budget* gate rather than on a warm
game land within a hundredth of a point of this tree's figure. The three it
measured on a warm game land two to eleven times larger there. That is the size
of the instrument gap, not disagreement between the trees — and it is the reason
a change rejected for reading −0.03% on this gate may still be worth a warm-game
measurement this tree does not yet have.

### Call counts, not costs, are the parity test

`perf_fingerprint.py --calls` answers "do we run Stockfish's algorithm?" — call
counts are inlining-immune, costs are not. Group on the symbols that exist in
*your* build: clang inlines upstream's affine layers into `Network::evaluate`
while mcfish keeps `nnue_affine_32` as a symbol, and upstream has two `do_move`
overloads. A regex written against the wrong side reads a divergence that is not
there.

The two differentials that read a VALUE rather than a cost — `upstream-parity` on
the node total and `upstream-transcript` on the bytes — are
[10-tooling-ci.md](10-tooling-ci.md)'s.

## Where the two engines stand on the SPINE, measured

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

## Strength testing, and what a match can actually resolve

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

## Debugging a divergence the gates cannot see

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

## A counter is a hypothesis until it is validated

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

## Nothing here measured more than one thread

Every speed axis on this page runs `Threads 1` — `nps_ab.sh`, `perf-budget`,
`perf-budget-tb`, `perf_counters.sh`, `perf-decomp` — and the Elo matches default to
it. A player runs eight or sixteen. So until
[`../tools/nps_threads.sh`](../tools/nps_threads.sh) landed, nothing in this tree
could see whether a change contends worse on the shared last level, on the
transposition table, or on the counters the manager polls.

**The other axes cannot simply be pointed at more threads.** Each refuses a
comparison whose node counts differ, and is right to. But a multi-threaded search at
a **fixed depth is not reproducible against itself**. Three runs of `bench 128 8 10`
on this tree:

```
3,773,312    6,144,045    4,460,748        a 62.8% spread
```

against a 0.02% tolerance. Every existing gate reports VOID, correctly, and learns
nothing.

**Make the node count the input instead of the output.** `bench <hash> <threads> <N>
default nodes` gives every position the same budget, and the same three runs read

```
9,849,966    9,854,910    9,852,636        a 0.05% spread
```

Lazy-SMP threads still overshoot a budget slightly and by a different amount each
run, so the node check here is a **band** rather than equality — the one place in this
tree where that is true, and it is a property of threaded search rather than a
concession.

**Read `r(T)/r(1)`, never the A/B column.** The ratio at T threads carries the
single-thread speed difference inside it, so a binary that is merely faster looks like
it scales better. Dividing by the ratio at one thread removes exactly that, and what
is left is the only column that answers whether the two **scale** differently. A
spread with no trend means nothing changed.

It does **not** pin a core, where `nps_ab.sh` does: contention between N threads is the
subject.

## Isolate the component instead of attributing it

**`perf-decomp` is the axis that attributes DIRECTLY**, and it is the one to reach for
first when a total moved and the question is which part moved it. It runs
[`../tools/perf_decomp.sh`](../tools/perf_decomp.sh) over two binaries under callgrind
with the cache and branch simulators on, sums **self** cost per symbol — the line after
a `calls=` line is the callee's inclusive cost and is skipped, or the whole NNUE
evaluation would be counted inside the search and again inside itself — and groups the
symbols by [`../tools/perf_components.tsv`](../tools/perf_components.tsv).

Two properties make it worth callgrind's order-of-magnitude slowdown, and they pull in
opposite directions. **It is deterministic**: two runs of one binary give identical
counts, so a component difference of any size is real rather than thermal, which is why
the depth stays small. **And it is a model**: the cache simulator is a fixed two-level
geometry with no prefetcher and no out-of-order execution. It ranks locality; it does
not predict time. Where it and `perf_counters.sh` disagree, that one is measuring the
hardware and this one a model of it.

Its first run localised a known change exactly — the low-ply hoist out of the quiet
scoring loop read `movepick 130.5M → 122.9M, 0.9417` with **every other row at
1.0000**, against a whole-program 0.9968.

Three rules the components file carries, each of which is a way to be wrong:

- **Rows are tried in order and the first match wins**, so a narrow row must precede
  the wider one that would swallow it.
- **A row matching nothing on both sides is reported BY NAME, never printed as a
  zero** — a zero reads as a total win forever. It means the workload did not exercise
  it (the tablebase rows on a bench-list run, which is why the bench list needs the
  probing workload to reach them) or the symbol stopped surviving inlining. There is no
  `tt` row for exactly that reason: clang inlines `tt_probe` and `tt_save` into the
  search at every tier here, so their work is inside `search nodes`.
- **A row matching on ONE side only divides a real cost by nothing.** It is marked `X`,
  excluded from the verdict and the run exits 1 — the other rows still stand, because
  asymmetric inlining is the expected outcome of the refactors this axis measures.

**Startup is its own rows**, and that is not bookkeeping: at a small depth the net
parse and the magic-table init are the largest rows in the profile, 17.6% and 14.4% of
a depth-7 run. A table that folded them into one total would report a search ratio that
is mostly startup.

Two more workloads answer questions no profile of the full engine can, and neither needs
an attribution argument because each simply removes what it is not measuring:

- **`perft`** is the board zone — movegen, make/unmake, legality, threats — with no
  TT, no histories, no move ordering and no evaluation. It reports its own node count,
  so tree identity is checked for free.
Read the PER-NODE columns beside every ratio, not the ratio alone. A ratio with no
base cannot say whether it matters: this session's whole-engine `cache misses 1.007`
reads like a finding until the absolutes turn out to be 57.0 against 57.4 per node,
which is nothing. Per NODE rather than per run is what also makes two transcripts
over DIFFERENT trees roughly comparable — a material-eval spine run against a
full-engine one — which they are not on absolutes.

- **`MCFISH_EVAL_MATERIAL=1`** replaces the evaluation with a material sum, which
  leaves the spine and the search running over a tree the network no longer shapes.
  Patch the oracle with the same formula — the weights are written out in both, not
  read from either engine's tables, precisely so the two cannot drift — and **assert
  the node counts match before believing anything**. The patch is
  [`../tools/material_eval.patch`](../tools/material_eval.patch), which also carries
  the apply-measure-restore sequence; a dirty oracle silently corrupts
  `upstream-parity`, `golden-audit` and `tb-update`, so restoring it is part of the
  procedure and not a tidy-up afterwards.

  `./build.sh material-eval [arch]` builds the pair and is what to use: applying the
  patch is the easy half, and putting the oracle back is the half that gets skipped.
  Reverting the source is NOT enough -- the stubbed binary stays on disk and its
  `.built-sha` stamp still matches the pin, so `upstream_oracle.sh` sees a current
  build and does nothing. The step's EXIT trap therefore deletes the binary and the
  stamp too: a missing oracle fails loudly on the next use, a stubbed one does not.

  The counts themselves are not quoted here. They are a function of the anchor and
  move with every sync — this page pinned three that were two syncs stale, which is
  the same rot the bench signature is banned from these pages for. Run the pair and
  compare; the assertion is that the two sides agree, not that they agree on a
  number written down once.

Running the same comparison over both is what localised the IPC gap to the search
rather than the board, in two commands and with no per-function attribution at all.

## The gates

Nothing on this page blocks a merge except the two budgets, and both are **LOCAL**:
their reference file is host- and toolchain-specific and is gitignored, so a fresh
clone reads 127 until someone records one. Every other row here is an instrument, not
a gate.

| step | what it proves here | owned by |
|---|---|---|
| `perf-budget` / `perf-budget-update` | an instruction-count regression the node signature is blind to, keyed by the ISA tier in the binary and held to 0.05% | this page |
| `perf-budget-tb` / `perf-budget-tb-update` | the same, over a PROBING workload, for the reader `perf-budget` cannot see at all | this page |
| `perf-decomp` | where the cost is, per component, deterministically — callgrind over both binaries, SELF cost per symbol | this page |
| `counter-validate` | that a counter means what its name says on *this* host | this page |
| `material-eval` | nothing — it is the ablation that isolates the spine from the network | this page |
| `fingerprint` | the ALGORITHM: that each group is CALLED as often here as upstream. **A call-count divergence outranks every cost finding.** | this page |
| `signature` | that both sides searched the same tree, without which every figure above is void | [10-tooling-ci.md](10-tooling-ci.md) |
| `arch-determinism` | that a number taken at one tier is a number about that tier and not about a divergence between them | [08-idiomatic-c.md](08-idiomatic-c.md) |

### `./build.sh perf-budget` and `perf-budget-update`

Measure retired instructions against `tools/instr_budget.golden`, keyed by the ISA
TIER in the binary (`MCFISH_ARCH_STRING`, so `native` is filed as the tier it
selected) and held to 0.05%. **LOCAL**: needs `perf_event_open`.

`perf-budget` measures the EXISTING `build/mcfish`. The stamp rebuild and the
tier-keyed budget close the old fake-regression trap between tiers; what is left is a
rebuild landing inside the timed step, so run `./build.sh build` first anyway.

### `./build.sh perf-budget-tb` and `perf-budget-tb-update`

The same measurement over a PROBING workload — `tools/cases/tb_probe.fens` at depth
14, with `SyzygyPath` composed into the bench file — filed under a `<tier>+syzygy`
row. It exists because `perf-budget` cannot see the tablebase reader at all: every
bench position has more men on it than any table `tb-fetch` installs, so
`registry.c`, `do_probe_table` and the decode loop are absent from that figure and a
bound inside them reads as free. It caught a 0.13% regression in a diagnostic that
`perf-budget` read as free.

**Run it on any edit under `src/platform/syzygy/`.** **LOCAL**, needs
`perf_event_open` *and* `./build.sh tb-fetch 5`; an incomplete corpus exits 127
loudly, because a probing measurement with no tables loaded is the bench list wearing
a different name. The corpus is 5-man, so every figure over it is a LOWER BOUND —
block count scales with table size.

### `./build.sh fingerprint`

[`../tools/upstream_fingerprint.sh`](../tools/upstream_fingerprint.sh) profiles both
engines under callgrind on one tree and asserts each group in
[`../tools/fingerprint_groups.tsv`](../tools/fingerprint_groups.tsv) is CALLED as
often here as upstream.

It holds the ALGORITHM, which every other differential is blind to. The anchor, the
goldens and the node differential all compare VALUES, so each passes over a state
divergence that happens not to move a node count on the positions it drives — and two
real defects were found by this and nothing else: a `ucinewgame` that discarded the
position, and a terminal root that skipped the per-worker reset, both surfacing as ONE
call of difference.

Inlining-immune, because a call count does not care how the callee was reached.
Deterministic, so a loaded box cannot flap it. **LOCAL** — needs valgrind and the
oracle; ~50x slow, so it is not in `parity`.

### `./build.sh counter-validate`

Drives [`../tools/perf_counter_validate.c`](../tools/perf_counter_validate.c)'s two
loops under the counter harness and asserts the **separation**: a latency-bound serial
chain must report IPC near 1, a throughput-bound one 3+.

Running the validator alone proves nothing — it prints one checksum. It is a
**workload**, and the measurement is what a counter says about it. **LOCAL**: needs
`perf_event_open`. Why an instrument is a hypothesis until it has answered a known
question is [above](#a-counter-is-a-hypothesis-until-it-is-validated).
