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
- **`ZFISH_BASE`** — the zfish SHA mcfish has been ported **up to** (`6a4a80887`).
  The three atomics fixes, the mirror drop, the `resources/` rename, the
  TSan-to-zero work and the intrinsic-translation docs are all reflected here
  (some via a different mechanism than zfish's — the C23 intrinsic doc, `build.sh`
  gates rather than `build.zig` steps). `074b972fe` is a fix to zfish's Zig test
  harness with no mcfish counterpart. `6a4a80887` (hoist the sparse-affine bases
  per nnz word) is already present: mcfish's unified `nnue_affine_32` hoists per
  word with a word-local bit index from the start, having never had zfish's
  per-tier `affineVnni` split to un-hoist.

  One zfish commit past this pin is genuinely NOT ported and cannot be a drop-in:
  `afd4f2d56` widens `transform_vec_width` 32→64. It is bench-neutral in zfish's
  loop structure and MOVES mcfish's anchor (`./build.sh signature` drifts) because
  mcfish's transform tile is a different shape. Taking it means restructuring the transform
  loop to stay behaviour-preserving, not flipping a constant — real NNUE work, and
  exactly the "improving while porting moves the node count" trap. Advance it in
  the commit that ports the last change from that range, not before: it is a record
  of what has landed here, not a bookmark of what was read.
- **`port_map.tsv`** — the work list: every zfish module → its mcfish owner → its
  Stockfish golden → status → risk tier.

**`./build.sh sync-status` is what checks these**, comparing each pin to its
checkout's `HEAD` and listing every commit in between. It reports; it does not
gate, because a tracked repository moving is normal — the failure it catches is
not noticing. Before this step existed nothing read the pins at all, and
`ZFISH_BASE` sat five commits stale while reading as authoritative.

## Tools

| script | what it does |
|---|---|
| `../port_status.sh` | progress report off `port_map.tsv`. Run via `./build.sh port-status`. |
| `upstream_oracle.sh [sha]` | builds **pristine** upstream at `sha` in a detached worktree and prints the binary path |
| `upstream_parity.sh [bin] [sha]` | the finish-line gate: mcfish bench vs the oracle bench. Run via `./build.sh upstream-parity`. |

## Why the oracle is a separate worktree

The oracle must be upstream's own code built by upstream's own Makefile, with no
mcfish edit anywhere near it. If the two shared a tree, a bug present in both
would cancel out and the gate would pass on a wrong engine. The worktree lives
outside the repo (`../.mcfish-upstream-oracle`) and is never committed.

## Why `upstream-parity` is not in `parity`

It is **red until the port completes**, by construction. `./build.sh parity` must
stay green on a correct in-progress tree, so the finish-line gate is a separate
step you run deliberately.

Day to day, `./build.sh signature` is the anchor: it pins mcfish's *current*
behaviour so a refactor cannot change it silently. That number is not the target
and never will be — see [../../docs/PORTING.md](../../docs/PORTING.md).

## Workflow

```sh
./build.sh port-status                      # what is left, by risk tier
# pick a module; read its zfish source and its Stockfish golden
$EDITOR ../zfish/src/<module>.zig
git -C ../Stockfish show HEAD -- src/<golden>
# translate into the mcfish owner named in port_map.tsv, then:
./build.sh parity                           # nothing already working may break
# flip the row's status in port_map.tsv, commit ONE module with the zfish source named
./build.sh upstream-parity                  # red until the last module lands
```
