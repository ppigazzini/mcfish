# Contributing to mcfish

mcfish is a **C23 port of [Stockfish][stockfish]** aiming at a bit-exact 1:1
clone. `../Stockfish` is the **golden** and the differential gate compares against
a pristine upstream build; where anything disagrees with Stockfish, Stockfish wins.

## Building

See the [README](README.md#build): install a **clang with C23 support** and run
`./build.sh`. There are no other dependencies.

## Faithfulness

**One logical change per commit.** A commit that touches three modules cannot be
bisected when the node count moves.

**Do not "improve" on upstream.** A cleaner formulation that moves
a rounding boundary moves the node count. Port faithfully first.

## The gates

Any change that touches engine behaviour must keep the whole battery green:

```
./build.sh parity
```

**Check its EXIT CODE, not a piped fragment.** `./build.sh parity | tail -1` reads
`0` from `tail` while the gate is red; that has laundered red gates in this port and
its sibling. Run it unpiped, or redirect and test `$?`.

**Two changes need a gate `parity` does not run:**

- **ISA-gated code** — `./build.sh arch-determinism`. The tiers run different
  algorithms now (slider attacks switch at avx2, move sorting at avx512, threat
  writing at ICL), so that step is what compares the vector path against the scalar
  path *on the same tree*. `signature` tests one tier and would pass over a wrong
  attack set at another.
- **Anything touching `platform/thread*.c`** — `./build.sh tsan`, and
  `./build.sh tsan-search` for the search itself. A race does not have to fire to
  be there.

All ten, in the order `parity` runs them:

| Gate | Asserts |
|---|---|
| `build` | the files in `SOURCES` compile under the full warning set |
| `zone-check` | `engine/` + `platform/` link with no `shell/` object |
| `fmt` | clang-format is clean |
| `docs-lint` | no dead doc links, no named paths that do not exist |
| `test` | the unit + property suite passes under ASan+UBSan |
| `signature` | `bench` reproduces `tools/signature.golden` |
| `simd-scalar` | the scalar SIMD fallback reproduces the same anchor as the vector path |
| `perft` | the reference positions in `tools/perft.table` match |
| `golden` | the UCI case outputs match `tools/*.golden` |
| `tb` | Syzygy discovery and the root probe match `tools/tb.golden` |

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
deliberately: it is what keeps every implicit integer conversion visible, since
the port depends on matching upstream's integer semantics exactly.

Comments are **imperative mood** and state the invariant the code cannot show —
see [docs/12-writing.md](docs/12-writing.md). Where **upstream** carries a comment
about integer semantics or cites a file and line, carry it across. Upstream is the
only tree that word means: `../zfish` is a sibling port, not a source this one
translates from (AGENTS.md, "The sibling").

For git blame, ignore the formatting-only revisions:

```
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

## Commits

One module per commit, as above.

Conventional subject, 72 characters or fewer; blank line; body wrapped at 80
carrying the evidence — gate output and exit code, not "should work". A change
that moves the bench signature must say what moved it.

**No trailers.** The body ends with the evidence and nothing after it:

- **no `Co-Authored-By:`** — not for a tool, not for an assistant, not
  automatically. A trailer naming a non-author is a false claim about who wrote
  the change, and `git log --format='%an'`, `git shortlog -sn` and every blame
  view repeat it forever.
- **no `Generated with …`**, and no tool advertisement of any kind.

This applies whoever or whatever is driving the commit. Tooling that appends a
trailer by default must be configured not to, rather than having it stripped
afterwards — the fix belongs before the commit, not in a later rewrite.

Commit locally and stop. Do not `git push` unless asked.

## License

By contributing you agree that your contributions are licensed under the **GNU
General Public License v3** — see [Copying.txt](Copying.txt) — the same license as
Stockfish, of which mcfish is a derivative.

[stockfish]: https://github.com/official-stockfish/Stockfish
