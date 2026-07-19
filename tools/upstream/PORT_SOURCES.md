# The three trees, pinned

mcfish is a C23 port. It has two reference trees, and they answer different
questions — so both are pinned here, and neither is optional.

| tree | role | commit |
| --- | --- | --- |
| `../Stockfish` | **golden.** Where zfish and mcfish disagree, this wins. | `ebcea3efe9c1b8748e080111c727c33c544d7e06` |
| `../zfish` | **port source.** A Zig port that is already bit-exact; templates, classes and RAII are already gone, which is why mcfish ports from here rather than from C++. | `054299636fd1a6955b88210fc063ff65bc8cba85` |

`tools/upstream/UPSTREAM_BASE` carries the Stockfish sha on its own, because
`upstream_oracle.sh` reads it. This file is the whole picture.

## Both ports are now pinned to the same upstream commit

zfish's own `tools/upstream/UPSTREAM_BASE` is also `ebcea3efe`, so the two ports
are level. They were not before: mcfish sat nine commits ahead of zfish, none of
which carried a `Bench:` line, which is why both benched the same number anyway.

That will drift again. When it does, the question is not how far apart they are
but whether a **bench-mover** sits between them:

```sh
git -C ../Stockfish log --format='%b' <zfish-base>..<mcfish-base> | grep '^Bench:' | sort -u
```

A non-empty result means the two no longer share a signature, and any
mcfish-vs-zfish node comparison is void until one of them moves.

## Rebuilding the oracle

`tools/upstream/upstream_oracle.sh` checks the golden out into a detached worktree
and builds it. Pass `--verify` — it asserts the built binary benches the commit's
own declared `Bench:`, and without it a stale or locally-edited worktree benches
wrong and every number taken afterwards is fiction.

The oracle is built at `ARCH=x86-64-sse41-popcnt` with the default compiler. That
is correct for **node counts**, which are compiler-independent. It is wrong for any
**instruction or cost ratio**: comparing a gcc-built oracle against a clang-built
mcfish measures the compilers. For that, build the reference with the same
toolchain, and see `MCFISH_ARCH` in `build.sh` for holding the ISA tier constant too.
