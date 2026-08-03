# Upstream tracking

mcfish targets a **bit-exact 1:1 clone of Stockfish**. This directory holds the
state and the tooling that makes that checkable.

## The repos

| Role | Path | Use |
|---|---|---|
| **Golden** | `../Stockfish` | the definition of correct behaviour |
| **Sibling** | `../zfish` | an independent Zig port of the same golden |
| **Sibling** | `../rfish` | an independent Rust port of the same golden |
| **Sibling** | `../fcfish` | an independent C17 port of the same golden |

**Only one of these is an authority.** The differential gates compare mcfish
against a **pristine upstream build**, never against a sibling. Where mcfish and
Stockfish disagree, Stockfish wins; where a sibling and Stockfish disagree, that
is a bug report for the sibling.

### A sibling finding is a hypothesis, not a bug report

Every rule below is written about zfish because that is where the pattern was
paid for; each applies unchanged to `../rfish` and `../fcfish`. One rule the
rfish sweeps added: **probe the named behaviour against the oracle before writing
a fix.** Of the eleven behaviours the fourth rfish sweep named, seven were already
correct here — the four that were not became `2ccd9c5a`, `d71ad444`, `8415fd6b`
and `f4f10372`, and the seven that were are recorded in those commit bodies so
the next sweep does not re-probe them. `git log --grep=rfish` finds the set.

### zfish is a sibling, not a source

mcfish began as a port OF zfish — a complete, bit-exact Zig port of Stockfish,
whose C++ templates, classes, RAII and operator overloading were already gone, so
Zig → C23 was close to mechanical. **That relationship is over.** The two are peer
ports of the same golden now, each syncing to Stockfish on its own schedule, and
the rules follow from that:

- **Neither tree is behind the other.** There is no commit range to absorb, so
  there is no pin here for zfish and `sync-status` does not mention it. Most of
  either log is language work — Zig std churn on one side, C23 idiom on the other
  — that will never have a counterpart. A status line calling that a deficit is a
  false alarm by construction. zfish does not pin mcfish either; it tracks
  Stockfish and refers to this tree as "the sibling C port" in prose.
- **Sweeps are bidirectional, and the direction is not obvious from the log.**
  The two cross-port constantly. In the 2026-08-01 sweep the numa insert, the
  `NumaPolicy` parse and the `setoption` grammar all turned out to flow
  mcfish → zfish — one of those zfish commits says in its own body that it was
  found by auditing mcfish. Ask what the sibling has that this tree should take
  AND what this tree has that the sibling should; answer both from the code.
- **Checking a SHA against the other tree's log is NOT the test.** Both reach the
  same wins independently and neither cites the other reliably: zfish `39bc445d`
  credits mcfish in its subject, while PEXT in `attacks.c` and the AVX2 maddubs
  tier in `simd.h` sit here plainly with no citation at all. Read the code.
- **A measurement does not transfer, in either direction.** This is symmetric and
  it is the trap that has cost the most time. mcfish's accumulator row tile is 64
  where zfish's is 128/256 (`f514ac56`, `2474792e`) because mcfish swept it here
  and 64 won. zfish's runBack inline won 1.0% there and measured FLAT here,
  because its gain was Zig's missing TBAA forcing reloads that clang+LTO never
  paid. A width or an inline is tuned for the language, tier and tree it was
  measured on. Re-measure or do not take it.
- **Refutations are the durable output of a sweep, and the commit log is where they
  live.** Each one is recorded in the body of the mcfish commit that measured it and
  refused it, so `git log --grep=zfish --grep=rfish` is the ledger — searchable, and
  it moves with the tree. Already refuted here: zfish `3d1ea031` runBack inline,
  `7b8a8b10` the THP skip, `0bfbf31c` the contiguous layer arena, the movepick
  pawn-history hoist, the fused refresh threat pass. `e15c4565` was reverted in
  zfish itself (`bb6fd153`). Search before re-deriving any of them.

There is no bookmark file for "how far the sibling has been read". A peer sweep
compares the two trees as they stand rather than replaying a range, and `git log
--grep=zfish` finds what the last one took. The 2026-08-01 sweep read
`f2299c47f..03c14b860` and landed the oracle golden audit and the tree-wide
`.gitattributes` rule; the 2026-07-24 audit closed `6a4a80887..f2299c47f`.

## State files

- **`UPSTREAM_BASE`** — the Stockfish SHA mcfish is porting to. The bench count of
  *this commit* is the finish line. Advance it only when `upstream-parity` is green.
- **`UPSTREAM_TARGET`** — the SHA being ported toward when catching up to a moving
  upstream; equal to `UPSTREAM_BASE` when synced.

**`./build.sh sync-status` is what checks these**, comparing the pin to the golden
checkout's `HEAD` and listing every commit in between, in BOTH directions — a
checkout sitting *before* the pin is red, because every grep of it then answers
from source this tree has already ported past. It reports; it does not gate,
because upstream moving is normal — the failure it catches is not noticing.
Before this step existed nothing read the pins at all, and one sat five commits
stale while reading as authoritative.

## Tools

| script | what it does |
|---|---|
| `upstream_oracle.sh [sha]` | builds **pristine** upstream at `sha` in a detached worktree and prints the binary path |
| `upstream_parity.sh [bin] [sha]` | the finish-line gate: mcfish bench vs the oracle bench. Run via `./build.sh upstream-parity`. |

## Why the oracle is a separate worktree

The oracle must be upstream's own code built by upstream's own Makefile, with no
mcfish edit anywhere near it. If the two shared a tree, a bug present in both
would cancel out and the gate would pass on a wrong engine. The worktree lives
outside the repo (`../.mcfish-upstream-oracle`) and is never committed.

## Why `upstream-parity` is not in `parity`

It needs a network fetch and a full pristine upstream build, which `./build.sh
parity` deliberately does not — so the finish-line gate is a separate step you run
deliberately.

Day to day, `./build.sh signature` is the anchor: it pins mcfish's behaviour so a
refactor cannot change it silently.
