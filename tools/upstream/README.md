# Upstream tracking

mcfish targets a **bit-exact 1:1 clone of Stockfish**. This directory holds the
state and the tooling that makes that checkable.

## The two repos

| Role | Path | Use |
|---|---|---|
| **Port source** | `../zfish` | the code you translate, module for module |
| **Golden** | `../Stockfish` | the definition of correct behaviour |

zfish is a complete, bit-exact **Zig** port of Stockfish. Port from it: the C++
templates, classes, RAII and operator overloading are already gone, the engine is
decomposed into small modules, and the result is proven bit-exact. Translating
Zig → C23 is close to mechanical.

The differential gate compares mcfish against a **pristine upstream build**, never
against zfish. Where zfish and Stockfish disagree, Stockfish wins, and the
divergence is a bug report for zfish.

## State files

- **`UPSTREAM_BASE`** — the Stockfish SHA mcfish is porting to. The bench count of
  *this commit* is the finish line. Advance it only when `upstream-parity` is green.
- **`UPSTREAM_TARGET`** — the SHA being ported toward when catching up to a moving
  upstream; equal to `UPSTREAM_BASE` when synced.
- **`ZFISH_BASE`** — the zfish SHA mcfish has been ported **up to** (`f2299c47f`).
  It is a record of what has landed here, not a bookmark of what was read, so
  advance it only behind an audit that says where each commit in the range went.

  The `6a4a80887..f2299c47f` range was audited commit by commit on 2026-07-24 and
  is closed. Most of it is not a port at all: mcfish and zfish cross-port, and the
  two trees reached the same wins independently — zfish's `39bc445d` credits
  mcfish in its own subject. Checking a zfish SHA against mcfish's log is
  therefore NOT the test; several wins are present without ever citing one (PEXT
  in `attacks.c`, the AVX2 maddubs affine tier in `simd.h`). Read the code.

  Four outcomes cover the range: already present independently (the transform
  widths, PEXT, the maddubs tier, the fused activations, the OUT==1 fc_2 dot, the
  vectorized history fill, mmap'd weights, the sized TT probe, uninitialized large
  pages); ported earlier with the SHA cited (the 30-commit round in
  `__DEV/PERFORMANCE.md`); refuted here with numbers (`3d1ea031` runBack inline,
  `7b8a8b10` the THP skip, `0bfbf31c` the contiguous layer arena, and the movepick
  pawn-history hoist — see the ledger); or measured-different-and-kept, which is
  the case that is easy to mistake for a gap. mcfish's accumulator row tile is 64
  where zfish's is 128/256 (`f514ac56`, `2474792e`) because mcfish swept it here
  and 64 won; a width is tuned for the tier and tree it was measured on, never
  inherited. `e15c4565` was reverted in zfish itself (`bb6fd153`) and must not be
  taken.

**`./build.sh sync-status` is what checks these**, comparing each pin to its
checkout's `HEAD` and listing every commit in between. It reports; it does not
gate, because a tracked repository moving is normal — the failure it catches is
not noticing. Before this step existed nothing read the pins at all, and
`ZFISH_BASE` sat five commits stale while reading as authoritative.

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
