# mcfish Developer Documentation

## Overview

mcfish is a **C23 port of Stockfish**, written against the full modern C23
feature set. The goal is a **bit-exact 1:1 clone**: the same `bench` node
signature, the same bestmove, NNUE evaluation, Syzygy tablebases, Lazy-SMP
threading and NUMA. Like Stockfish it is a UCI engine, not a GUI.

**The port is in progress.** Run `./build.sh port-status` for the live figure;
read [PORTING.md](PORTING.md) before writing any engine code.

**`../Stockfish` is the golden.** It defines correct behaviour, the differential
gate compares against a pristine upstream build, and every module here cites the
upstream file and line it was translated from. Where this tree and Stockfish
disagree, Stockfish wins. [PORTING.md](PORTING.md) describes the sources a port
is written from.

### The one structural fact to hold before reading anything else

**Most ported `.c` files in this tree are not in the binary.**
[`../build.sh`](../build.sh) has no dependency scanner and no wildcard: its
`SOURCES` array is the complete list of translation units the release binary is
built from, and `ENGINE_SOURCES` is what `zone-check` and the test binary link.
A file that is not in those arrays compiles nowhere, links nowhere, and is
covered by **no gate** — not `signature`, not `perft`, not `golden`, not `test`.

One ported subsystem is in that state today: the decomposed shell in
`src/shell/engine.c`, a dead duplicate of the live option table in `uci.c`.

A second failure mode sits one step past it and reads the same from the outside: a
module that IS in `SOURCES`, compiles, links and is unit-tested, but that nothing in
the engine calls. The thread pool and the NUMA runtime are there now — gated, but not
driven by the search. Each page below says which of its
modules are wired and which are not.

**That is a gap, not a design.** A module verified in isolation and left out of
`SOURCES` is a module nothing defends against the next edit. Wiring it is part of
the port, not a follow-up, and each page names what the wiring commit has to
decide.

The repository holds two things:

- **The engine** — the runtime in C, split into three zones: `src/engine/` (the
  chess library: board, movegen, search, state, evaluation), `src/platform/` (the
  OS runtime), and `src/shell/` (the process: UCI, bench, `main`). Every module
  has a row in `tools/upstream/port_map.tsv` naming its Stockfish golden and its
  status.
- **The tooling** — [`../build.sh`](../build.sh) is the whole build system: one
  clang invocation per step, no Makefile and no generator. It also carries the
  gate battery — the bench anchor, the perft table, the UCI golden-diff cases,
  the zone check, the format check and the docs lint — and `tools/` holds the
  files those gates read.

### Naming code that is not in this tree

These pages routinely name modules that live in Stockfish rather than here.
Written bare, a path under `src/` is a claim that the file exists **here**, which
`./build.sh docs-lint` checks. So throughout this set a Stockfish golden is
written relative to Stockfish's `src/`, as *upstream `nnue/network.cpp`*.

The mcfish owner of each golden, plus its status, is
`tools/upstream/port_map.tsv`.

## Documents

| Document | Audience | Description |
|---|---|---|
| [PORTING.md](PORTING.md) | Anyone writing engine code | The goal, the port sources, the golden, the M1..M6 milestones and the gate that ends each one |
| [00-architecture.md](00-architecture.md) | All contributors | The three zones, the dependency direction, how `zone-check` enforces it at link time, the composition root and its init order, what is wired into the binary and what is not, how one search flows |
| [01-engine-board.md](01-engine-board.md) | Engine contributors | Types and the 16-bit move encoding, the bitboard leaf and the magic slider tables, Position/StateInfo, Zobrist, the threat deltas, FEN, move generation and legality |
| [02-engine-search.md](02-engine-search.md) | Engine contributors | Iterative deepening, alpha-beta and qsearch, the staged move picker and the history block, the pruning set, the cluster transposition table, time management and determinism, the unwired search decomposition |
| [03-engine-eval.md](03-engine-eval.md) | Engine contributors | The NNUE evaluation, the accumulator bracket every make/unmake owes it, the net load path, and the classical fallback that is still scaffolding |
| [04-multithreading.md](04-multithreading.md) | Engine and platform contributors | Lazy-SMP as upstream runs it, the pool and worker lifecycle, shared versus per-worker state, memory ordering, NUMA — all of it compiled and gated, none of it driven, and what the wiring commit has to decide |
| [05-tablebases.md](05-tablebases.md) | Engine and platform contributors | The Syzygy prober end to end: the file format, loading and its concurrency, the WDL and DTZ probes, the in-search and root integrations, the PV extension, the four options, and what the `tb` gate does and does not cover |
| [06-platform.md](06-platform.md) | Platform contributors | `src/platform/`: what is wired, the monotonic clock, the feature-test macro and the engine→platform edge; the thread/NUMA and Syzygy subsystems have their own pages above |
| [07-shell.md](07-shell.md) | Shell contributors | `main` as the composition root, every UCI command the live loop handles, the option tables, the injected output sink, bench |
| [08-idiomatic-c.md](08-idiomatic-c.md) | Hot-path and build contributors | The C23 patterns this repo commits to, the warning set, why there is no build system, the recurring porting patterns, the measurement discipline |
| [09-tooling-ci.md](09-tooling-ci.md) | All developers | Every `./build.sh` step and what it gates, the source arrays that decide what is gated at all, the golden-diff harness and its normalization, fact tables versus goldens, the anchor versus the finish line, the CI lanes |
| [10-references.md](10-references.md) | All developers | Stockfish, chess-domain, C23, Syzygy and NNUE references |
| [11-writing.md](11-writing.md) | Anyone editing these docs | How the set is organised, the writing rules, the hot/cold map, code-comment style, and what `docs-lint` cannot check |

## Quick start

Requires **clang with C23 support** and a POSIX host. There are no other build
dependencies; `clang-format` is needed only by the `fmt` step.

```bash
./build.sh                  # build the release binary -> build/mcfish
./build.sh net              # where the NNUE net must be, and how to obtain it
./build.sh test             # unit + property suite under ASan/UBSan
./build.sh bench            # run the bench and print the node total
./build.sh signature        # assert the bench total vs tools/signature.golden
./build.sh parity           # the whole in-repo gate battery
./build.sh port-status      # progress toward bit-exactness
./build.sh help             # every step
```

The net is a **runtime input, not a build product**, and lives in `resources/`
beside the Syzygy tables. `./build.sh net` reports and never downloads, so a clean
build never depends on the network. Every `./build.sh` step that runs the engine
runs it **from `resources/`**, which is how the file is found: the engine itself
searches upstream's three candidates and no others, and the working directory is
the second. Without a net the engine still runs, on the classical fallback
described in [03-engine-eval.md](03-engine-eval.md), and says so through
`info string`.

`./build.sh parity` is what to run before calling a byte-changing change done. It
builds, zone-checks, format-checks, docs-lints, tests, and asserts the signature,
the perft table and the UCI goldens. A gate whose tool is missing exits 127 and is
**skipped, not passed** — `parity` names each one it skipped. Note the limit that
follows from `SOURCES`: every one of those gates runs over the linked binary, so
none of them sees a ported file that is not in the array.

## Technology

| Layer | Technology |
|---|---|
| Language | C23; `build.sh` probes for `-std=c23` and falls back to `-std=c2x`, never to an older mode |
| Compiler | clang, with a gcc second-compiler lane held to the same anchor; the binary reports its own version via the UCI `compiler` command |
| Build | [`../build.sh`](../build.sh) — an enumerated source list and one clang call per step; no Makefile, no CMake, no dependency tracking |
| Warnings | `-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion`, with `-Wno-unused-parameter` the only suppression |
| Sanitizers | ASan + UBSan on the `debug` and `test` steps |
| Slider attacks | magic bitboards in [`../src/engine/board/attacks.c`](../src/engine/board/attacks.c), built at startup by `attacks_init` |
| Evaluation | NNUE, under `src/engine/eval/nnue/`, with an incremental accumulator the search brackets. The net is a runtime input; a build with no net falls back to a classical material + PSQT term that is **scaffolding** |
| Search | single-threaded iterative-deepening alpha-beta with quiescence, a staged move picker, the history block and the time manager. The thread pool is in the build and gated, but nothing drives it: the search's per-worker state is still file-scope globals (M4) |
| Endgames | Syzygy WDL/DTZ probing, wired: the prober in `src/platform/syzygy/`, the root ranking and the Step 6 in-search probe. Tables are a runtime input — with no `SyzygyPath` the engine never probes |
| Protocol | UCI |

The rows marked unwired or absent are milestones in [PORTING.md](PORTING.md), not
scoping decisions. The engine is not a Stockfish clone without them.

## Project layout

```
mcfish/
|-- build.sh             -- the build, the SOURCES arrays, and the whole gate battery
|-- src/
|   |-- engine/          -- the chess library
|   |   |-- board/       -- types, bitboards, magic attacks, position, movegen,
|   |   |                   zobrist, threats, repetition, FEN, legality, UCI moves
|   |   |-- search/      -- the live search.c and its decomposition, movepick,
|   |   |                   history, timeman, the TT
|   |   |-- state/       -- per-worker layout, shared state, root moves, position
|   |   |                   storage, limits (built and tested; not driven yet)
|   |   `-- eval/        -- the dispatch and classical fallback, and nnue/ (the network)
|   |-- platform/        -- the OS runtime: the monotonic clock, syzygy/ (the
|   |                       tablebase prober), and thread, NUMA and memory
|   |                       (built and tested; the pool is not driven yet)
|   `-- shell/           -- main (composition root), the live uci.c, bench, and
|                           engine.c (a dead duplicate of uci.c's option table)
|-- resources/           -- the external runtime inputs: the NNUE net and
|                           syzygy/ (the tablebases). Fetched, never committed;
|                           every ./build.sh step runs the engine from here
|-- tools/               -- the gate inputs, and upstream/ (the port map and the SHA pin)
|-- tests/               -- test_main.c (mcfish-owned) plus upstream Stockfish mirrors
|-- scripts/             -- upstream Stockfish mirrors
|-- docs/                -- this documentation
`-- Copying.txt, AUTHORS -- GPL v3; Stockfish attribution
```

Read "not driven yet" as the gap it is, and do not mistake it for the older one:
`build.sh` names those files, so they compile under the full warning set and the
tests reach them — but nothing in the engine calls them, so no gate can tell whether
they would still be *correct* when it does.

`src/`, `build.sh`, `tools/`, `tests/test_main.c` and `docs/` are
mcfish-owned. The rest of `tests/` and `scripts/net.sh` are upstream Stockfish
mirrors, kept verbatim so a future rebase is a copy rather than a merge; they do
not run against mcfish today and are not edited here.
