# The oracle, and where the port started

This page is about building the reference binary the differential gates compare
against, and about one historical SHA. **The live pin is
[`UPSTREAM_BASE`](UPSTREAM_BASE)**, which `upstream_oracle.sh` reads; `./build.sh
sync-status` is what checks it against the golden checkout.

## There is one reference tree, not two

This file used to open with a table pinning `../Stockfish` **and** `../zfish`, and
calling the second one mcfish's "port source". Both halves of that are now wrong,
and it stayed wrong here for a month while [README.md](README.md) and
[AGENTS.md](../../AGENTS.md) said the opposite two directories away.

mcfish was first ported FROM zfish, and **that relationship is over.** zfish is one
of four peer ports of the same golden — with `../rfish`, `../fcfish` and
`../Stockfish`'s own `refish` branch — none of which is a source, none of which is
pinned here, and none of which `sync-status` mentions. The sweeps run in BOTH
directions and the log does not say which. [README.md](README.md) carries those
rules and the evidence behind each one; do not re-derive them from this page.

The pin table is gone rather than corrected because a second copy of a number that
already lives in `UPSTREAM_BASE` is a number that can disagree with it, and did.

## Rebuilding the oracle

[`upstream_oracle.sh`](upstream_oracle.sh) checks the golden out into a detached
worktree and builds it. Pass `--verify` — it asserts the built binary benches the
commit's own declared `Bench:`, and without it a stale or locally edited worktree
benches wrong and every number taken afterwards is fiction.

It DETACHES `../Stockfish`'s own HEAD as a side effect. Put the branch back when
the sync is done; the branch itself is never touched.

The oracle is built at `ARCH=x86-64-sse41-popcnt` with the default compiler. That
is correct for **node counts**, which are compiler-independent. It is wrong for any
**instruction or cost ratio**: comparing a gcc-built oracle against a clang-built
mcfish measures the compilers. For that, build the reference with the same
toolchain, and see `MCFISH_ARCH` in [`../../build.sh`](../../build.sh) for holding
the ISA tier constant too.

## Where a port's decisions are written down

Not here. Each sync's pin-advance commit carries the whole range — what landed, what
had no counterpart, and why — because a hand-maintained register beside the code is
the thing this page just demonstrated rots. `git log --grep='chore(upstream)'` finds
them, and `git log --grep=zfish --grep=rfish --grep=refish` finds what each sibling
sweep took and what it measured and refused.

A part of an upstream commit that needed nothing here is recorded there too. A
commit half-ported and silently is how a port stops being one.

## The one SHA on this page

mcfish's port began at Stockfish `ebcea3efe` (*VVLTC parameters tweak*,
2026-07-16). It is written short on purpose: `docs_cite.sh` matches a backticked
7–12 digit token, so the 40-character spelling this page used to carry was checked
by nothing. It is kept as provenance, and it is the
tree's **only** citation that is off-branch: an object in this clone, reachable
from a ref, an ancestor of nothing here. That is the third of
[`docs_cite.sh`](../docs_cite.sh)'s four tiers, and
[13-writing.md](../../docs/13-writing.md) names this page as its example —
`cite-check` reports it as `1 off-branch but reachable`.
