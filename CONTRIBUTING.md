# Contributing to mcfish

mcfish is a **C23 port of [Stockfish][stockfish]** aiming at a bit-exact 1:1
clone. Read [docs/PORTING.md](docs/PORTING.md) first — it defines the goal, the
milestones, and where the code you are about to write comes from.

## Building

See the [README](README.md#build): install a **clang with C23 support** and run
`./build.sh`. There are no other dependencies.

## Port from zfish

`../zfish` is a complete, bit-exact **Zig** port of Stockfish. Port from it,
module for module. `../Stockfish` is the **golden** — it defines correct
behaviour, and the differential gate compares against a pristine upstream build.
Where the two disagree, Stockfish wins.

`tools/upstream/port_map.tsv` is the work list. `./build.sh port-status` prints
where the port stands.

**One module per commit**, naming the zfish source in the body. A commit that
ports three modules cannot be bisected when the node count moves.

**Do not "improve" on zfish or upstream while porting.** A cleaner formulation
that moves a rounding boundary moves the node count. Port faithfully first.

## The gates

Any change that touches engine behaviour must keep the whole battery green:

```
./build.sh parity
```

| Gate | Asserts |
|---|---|
| `zone-check` | `engine/` + `platform/` link with no `shell/` object |
| `fmt` | clang-format is clean |
| `docs-lint` | no dead doc links, no named paths that do not exist |
| `test` | the unit + property suite passes under ASan+UBSan |
| `signature` | `bench` reproduces `tools/signature.golden` |
| `perft` | the reference positions in `tools/perft.table` match |
| `golden` | the UCI case outputs match `tools/*.golden` |

A gate whose **tool** is missing exits 127; `parity` names it as SKIPPED and does
not claim it passed. A skipped gate proves nothing — install the tool before
relying on the run.

## Two different numbers

Do not confuse them:

- **`tools/signature.golden`** — mcfish's node count *today*. It exists so a
  refactor cannot silently change behaviour mid-port. It is not the target.
- **Upstream's `Bench:` at `tools/upstream/UPSTREAM_BASE`** — the finish line.

## Regenerating a golden

`signature-update` and `golden-update` exist, and both are dangerous:

**Regenerating a golden on a red gate pins the defect.** The gate then passes
forever with the bug baked into the expectation. Before running either, establish
that the behaviour change is *intended*, and say in the commit body what moved and
why.

`tools/perft.table` is deliberately **not** a `.golden` and no step regenerates
it. Those node counts are facts about chess, identical for every correct engine,
so a mismatch is always a bug in mcfish.

## Code style

C code is formatted with `clang-format` (`./build.sh fmt-fix`). The build compiles
with `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion` and the tree is
warning-clean; keep it that way rather than suppressing. `-Wconversion` is on
deliberately: Zig makes every integer conversion explicit, and this is how those
conversions stay visible after translation to C.

Comments are **imperative mood** and state the invariant the code cannot show —
see [docs/09-writing.md](docs/09-writing.md). Where zfish carries a comment about
integer semantics or cites `upstream file:line`, carry it across.

For git blame, ignore the formatting-only revisions:

```
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

## License

By contributing you agree that your contributions are licensed under the **GNU
General Public License v3** — see [Copying.txt](Copying.txt) — the same license as
Stockfish, of which mcfish is a derivative.

[stockfish]: https://github.com/official-stockfish/Stockfish
