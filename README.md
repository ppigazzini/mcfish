# mcfish

**mcfish** is a **C23 port of the [Stockfish][stockfish] chess engine**, written
against the full modern C23 feature set. The goal is a **bit-exact 1:1 clone**: the same `bench` node
signature, the same bestmove, NNUE evaluation, Syzygy tablebases and Lazy-SMP
threading. Like Stockfish, it is a UCI engine, not a GUI.

It is a Lazy-SMP engine with NNUE evaluation that is **bit-exact with upstream** —
`./build.sh signature` benches the same node count Stockfish produces — with Syzygy
tablebases, Lazy-SMP threading and NUMA wired into `build.sh`'s `SOURCES` and driven
by the search.

## How it was ported

Stockfish's C++ leans on templates, classes, RAII, operator overloading and
exceptions, none of which map onto C23 without re-deciding the design. The port
makes those decisions once and decomposes the engine into small
single-responsibility modules.

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
./build.sh help         # every step
```

## Documentation

- [docs/](docs/README.md) — the architecture, each subsystem, the C23 patterns.
- [CONTRIBUTING.md](CONTRIBUTING.md) — the gates and the workflow.

## License

mcfish is a derivative of Stockfish and is distributed under the **GNU General
Public License v3** — see [Copying.txt](Copying.txt). All chess strength and the
NNUE networks come from the [Stockfish project][stockfish]; see
[AUTHORS](AUTHORS). The networks are trained on [Leela Chess Zero data][lc0-data]
under the [ODbL][odbl].

[stockfish]: https://github.com/official-stockfish/Stockfish
[lc0-data]:  https://storage.lczero.org/files/training_data
[odbl]:      https://opendatacommons.org/licenses/odbl/odbl-10.txt
