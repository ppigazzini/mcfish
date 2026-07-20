# mcfish

**mcfish** is a **C23 port of the [Stockfish][stockfish] chess engine**, written
against the full modern C23 feature set. The goal is a **bit-exact 1:1 clone**: the same `bench` node
signature, the same bestmove, NNUE evaluation, Syzygy tablebases and Lazy-SMP
threading. Like Stockfish, it is a UCI engine, not a GUI.

**The port is in progress.** Run `./build.sh port-status` for the live figure, and
read it carefully: it reports how many rows the port map *claims* alongside how many
are backed by code the binary actually contains, because those two numbers have been
far apart.

What exists today is a Lazy-SMP engine with NNUE evaluation that is
**bit-exact with upstream** — `./build.sh signature` benches the same node count
Stockfish produces. Syzygy tablebases, Lazy-SMP threading and NUMA are wired into
`build.sh`'s `SOURCES` and driven by the search; the main piece still on disk and
out of the binary is the decomposed shell. See [docs/PORTING.md](docs/PORTING.md).

## How it is being ported

Stockfish's C++ leans on templates, classes, RAII, operator overloading and
exceptions, none of which map onto C23 without re-deciding the design. The port
makes those decisions once and decomposes the engine into small
single-responsibility modules; [docs/PORTING.md](docs/PORTING.md) records where
each module's code comes from.

`../Stockfish` remains the **golden**: it defines correct behaviour, and the
differential gate compares against a pristine upstream build. Where mcfish and
Stockfish disagree, Stockfish wins.

## Build

Requires **clang with C23 support** and bash. No build system, no dependencies.

```
./build.sh              # build the engine (-O3) -> build/mcfish
./build.sh test         # unit + property suite, ASan+UBSan
./build.sh bench        # run the benchmark and print the node signature
./build.sh parity       # the full in-repo gate battery
./build.sh port-status  # progress toward bit-exactness
./build.sh help         # every step
```

## Documentation

- [docs/PORTING.md](docs/PORTING.md) — the milestones, the port map, the rules.
- [docs/](docs/README.md) — the architecture, each subsystem, the C23 patterns.
- [CONTRIBUTING.md](CONTRIBUTING.md) — the gates and the workflow.
- `tools/upstream/port_map.tsv` — the module-by-module work list.

## License

mcfish is a derivative of Stockfish and is distributed under the **GNU General
Public License v3** — see [Copying.txt](Copying.txt). All chess strength and the
NNUE networks come from the [Stockfish project][stockfish]; see
[AUTHORS](AUTHORS). The networks are trained on [Leela Chess Zero data][lc0-data]
under the [ODbL][odbl].

[stockfish]: https://github.com/official-stockfish/Stockfish
[lc0-data]:  https://storage.lczero.org/files/training_data
[odbl]:      https://opendatacommons.org/licenses/odbl/odbl-10.txt
