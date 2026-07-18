# ccfish

**ccfish** is a **C23 port of the [Stockfish][stockfish] chess engine**, written
against the full modern C23 feature set. The goal is a **bit-exact 1:1 clone**: the same `bench` node
signature, the same bestmove, NNUE evaluation, Syzygy tablebases and Lazy-SMP
threading. Like Stockfish, it is a UCI engine, not a GUI.

**The port is in progress.** Run `./build.sh port-status` for the live figure —
what exists today is a correct single-threaded engine with a placeholder
evaluation, and most of the engine is unported. See [docs/PORTING.md](docs/PORTING.md).

## How it is being ported

From **zfish** (the sibling checkout at `../zfish`) — already a **complete,
bit-exact Zig port of Stockfish** — rather than from Stockfish's C++ directly.

Stockfish's C++ leans on templates, classes, RAII, operator overloading and
exceptions, none of which map onto C23 without re-deciding the design. zfish has
already made every one of those decisions, decomposed the engine into small
single-responsibility modules, and proven the result bit-exact. Translating that
Zig into C23 is close to mechanical.

`../Stockfish` remains the **golden**: it defines correct behaviour, and the
differential gate compares against a pristine upstream build. Where zfish and
Stockfish disagree, Stockfish wins.

## Build

Requires **clang with C23 support** and bash. No build system, no dependencies.

```
./build.sh              # build the engine (-O3) -> build/ccfish
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

ccfish is a derivative of Stockfish and is distributed under the **GNU General
Public License v3** — see [Copying.txt](Copying.txt). All chess strength and the
NNUE networks come from the [Stockfish project][stockfish]; see
[AUTHORS](AUTHORS). The networks are trained on [Leela Chess Zero data][lc0-data]
under the [ODbL][odbl].

[stockfish]: https://github.com/official-stockfish/Stockfish
[lc0-data]:  https://storage.lczero.org/files/training_data
[odbl]:      https://opendatacommons.org/licenses/odbl/odbl-10.txt
