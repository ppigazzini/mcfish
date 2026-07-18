# AGENTS.md

ccfish is a **C23 port of Stockfish**, built with clang by `./build.sh`. The goal
is a **bit-exact 1:1 clone** — same bench signature, NNUE, Syzygy, Lazy-SMP.

**Read [docs/PORTING.md](docs/PORTING.md) before writing any engine code**, then
[docs/](docs/README.md) for the architecture. [CONTRIBUTING.md](CONTRIBUTING.md)
has the workflow. This file is only what an agent gets wrong before it has read
either.

## The one thing agents get wrong here

**Port from `../zfish`, not from `../Stockfish`.**

zfish is a complete, bit-exact **Zig** port of Stockfish. Its templates, classes,
RAII and operator overloading are already gone, it is decomposed into small
modules, and it is proven bit-exact. Translating Zig → C23 is close to mechanical;
translating C++ → C23 means re-making every design decision zfish already made.

`../Stockfish` is the **golden** — it defines correct behaviour and the
differential gate compares against it. Where the two disagree, Stockfish wins.

`tools/upstream/port_map.tsv` maps every zfish module to its ccfish owner, its
Stockfish golden, and its status. `./build.sh port-status` prints the live counts.

## The port is unfinished, and that is not a design

Do not document, gate, or optimise around the current shape as if it were
intended. Every one of these is **required and unported**, not a scoping decision:

- **NNUE** — `src/engine/eval/evaluate.c` holds a classical material+PSQT
  placeholder. It is **scaffolding to be deleted**. Do not tune it, extend it, or
  give it callers NNUE will not satisfy.
- **Syzygy tablebases** — unported.
- **Lazy-SMP threading and NUMA** — unported; the search is single-threaded and
  the UCI `Threads` option says so on stdout.
- **Magic bitboards** — sliders ray-cast in `attacks_bb`; correct but slow.
- **The option model** — the live UCI layer advertises a hand-written subset of
  upstream's option table, and the search answers its own option seam with
  upstream's defaults because nothing else can.

The bench signature in `tools/signature.golden` is **ccfish's current count, not
the target**. It exists so a refactor cannot silently change behaviour today. The
finish line is upstream's `Bench:` at `tools/upstream/UPSTREAM_BASE`. Do not
confuse them.

## Setup

```sh
./build.sh              # binary is `ccfish`, at build/ccfish
./build.sh help         # every step
./build.sh parity       # the aggregate gate — run before calling anything done
./build.sh port-status  # how far from bit-exact
```

There is **no Makefile and no build system**. A new `.c` file must be added to
`SOURCES` in `build.sh` — and, if it belongs to `engine/` or `platform/`, to
`ENGINE_SOURCES` too, or `zone-check` and the test binary will not see it.

## Gates

**A behaviour-changing edit is not done until a gate says so.**

```sh
./build.sh parity       # build, zone-check, fmt, docs-lint, test, signature, perft, golden
./build.sh signature    # just the anchor
./build.sh test         # unit + property suite, ASan+UBSan
```

`parity` names any gate it skipped for a missing tool. A skipped gate proves
nothing — never report it as a pass.

## Traps that cost real time

| trap | where |
|---|---|
| Porting from Stockfish's C++ instead of zfish's Zig. Slower, and re-decides what zfish settled. | [docs/PORTING.md](docs/PORTING.md) |
| `signature-update` / `golden-update` on a **red** gate launders a bug into the anchor. Fix the code, then re-derive. | [CONTRIBUTING.md](CONTRIBUTING.md) |
| `tools/perft.table` is **not** a golden. Those counts are facts about chess; a mismatch is always a movegen bug. | [CONTRIBUTING.md](CONTRIBUTING.md) |
| "Improving" on zfish or upstream while porting. A cleaner formulation that moves a rounding boundary moves the node count. | [docs/PORTING.md](docs/PORTING.md) |
| Integer semantics differ across C++/Zig/C at the edges, and upstream relies on wrapping in places. | [docs/PORTING.md](docs/PORTING.md) |
| Comments are **imperative mood**; never pin a number a gate computes. | [docs/README.md](docs/README.md) |

## Commits

**One module per commit**, naming the zfish source in the body — a commit that
ports three modules cannot be bisected when the node count moves.

Conventional subject ≤72 chars, blank line, body wrapped at 80 carrying the
evidence: gate output and exit code, not "should work". **Don't** `git push` —
commit locally and stop unless asked. **Don't** add co-author or generated-by
trailers.
