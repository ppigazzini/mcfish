# AGENTS.md

mcfish is a **C23 port of Stockfish**, built with clang by `./build.sh`. The goal
is a **bit-exact 1:1 clone** — same bench signature, NNUE, Syzygy, Lazy-SMP.

Read [docs/](docs/README.md) for the architecture, and
[CONTRIBUTING.md](CONTRIBUTING.md) for the workflow. This file is only what an agent
gets wrong before it has read either.

## The golden

`../Stockfish` is the **golden** — it defines correct behaviour and the differential
gate compares mcfish against a pristine upstream build. Where mcfish and Stockfish
disagree, Stockfish wins.

## Known limitations

Do not document, gate, or optimise around the current shape as if it were the
intended end state. Check the state against the tree before acting on it — the
reliable test is whether a file appears in `build.sh`'s `SOURCES`, because a module
outside it is unwired, not deferred:

- **Syzygy tablebases** — wired. All six `src/platform/syzygy/` files plus
  `tablebase.c` are in `SOURCES` **and** `ENGINE_SOURCES`, the four UCI options
  are live, and `./build.sh tb` gates discovery and the root probe against the
  oracle. `./build.sh tb-fetch` gets the 3-man set into `resources/syzygy/`;
  without it the gate checks
  discovery only and says so. The `d` command prints `Tablebases WDL:`/`DTZ:` lines
  once a `SyzygyPath` covers the position. Still open: no 5-man/cursed-win coverage.
- **Lazy-SMP threading and NUMA** — **wired.** `Threads` builds a worker set,
  `NumaPolicy` chooses the topology it binds under, and a `go` runs N workers over
  one root. `search_threads.c` is the driver; every piece of per-worker state that
  was a file-scope static now lives in the `SearchWorker` block, and the pool's
  totals reach the search only through `pool_source.h`, which answers with thread
  0's own values at `Threads 1`. Still open: the NNUE network does not register
  itself for NUMA replication, so a policy change re-partitions the threads without
  re-replicating any weights, and no unit test constructs a `SearchWorker`. See
  [docs/04-multithreading.md](docs/04-multithreading.md).
- **The option model** — the live UCI layer advertises a hand-written subset of
  upstream's option table, and the search answers its own option seam with
  upstream's defaults because nothing else can.
- **`go nodes N` overshoots by a different amount than upstream.** Both are
  deterministic and both stop on `nodes >= limit`; the counter scaling
  (`min(512, N/1024)`), the `check_time` call site in Step 1 and the `++nodes`
  point inside `do_move` were all compared and match. Measured from startpos:
  N=1000 agrees exactly, N=10000 gives 10011 against 10008, N=1000000 gives
  1000083 against 1000223 -- so the error is small and runs in BOTH directions.
  Nothing in the signature depends on it (the bench is depth-limited), but
  `bench <tt> <threads> <N> default nodes` will not match upstream's total.

The bench signature in `tools/signature.golden` is **upstream's number**, and
mcfish currently produces it — matching Stockfish at
`tools/upstream/UPSTREAM_BASE`, and matching zfish. It is a bit-exactness anchor,
not a local snapshot; run `./build.sh signature` for the value. A change that moves it is a behaviour change and must say
what moved it.

Bit-exactness is not the same as faithfulness, and the anchor cannot tell them
apart: the bench is a fixed position list, so a divergence that never fires on
those 51 positions is invisible to it. `tools/upstream_nodes.py` is the check that
is not fooled — it drives both engines over positions reached by random legal
moves, which appear in no bench list and no golden.

## Setup

```sh
./build.sh              # binary is `mcfish`, at build/mcfish
./build.sh help         # every step
./build.sh parity       # the aggregate gate — run before calling anything done
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
| `signature-update` / `golden-update` on a **red** gate launders a bug into the anchor. Fix the code, then re-derive. | [CONTRIBUTING.md](CONTRIBUTING.md) |
| `tools/perft.table` is **not** a golden. Those counts are facts about chess; a mismatch is always a movegen bug. | [CONTRIBUTING.md](CONTRIBUTING.md) |
| "Improving" on upstream. A cleaner formulation that moves a rounding boundary moves the node count. | [docs/08-idiomatic-c.md](docs/08-idiomatic-c.md) |
| Integer semantics differ across C++/C at the edges, and upstream relies on wrapping in places. | [docs/08-idiomatic-c.md](docs/08-idiomatic-c.md) |
| Comments are **imperative mood**; never pin a number a gate computes. | [docs/README.md](docs/README.md) |

## Commits

**One logical change per commit** — a commit that touches three modules cannot be
bisected when the node count moves.

Conventional subject ≤72 chars, blank line, body wrapped at 80 carrying the
evidence: gate output and exit code, not "should work". **Don't** `git push` —
commit locally and stop unless asked. **Don't** add co-author or generated-by
trailers.
