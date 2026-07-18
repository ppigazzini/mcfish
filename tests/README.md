# tests/

Two different things live here, and the distinction matters:

| Path | Owner | Edit here? |
|---|---|---|
| `test_main.c` | ccfish | **Yes** — this is the unit + property suite, run by `./build.sh test` |
| `instrumented.py`, `testing.py`, `perft.sh`, `reprosearch.sh` | upstream Stockfish | **No** — verbatim mirrors |

The upstream scripts are kept unmodified so a future rebase against Stockfish is a
clean copy rather than a merge. They target the upstream binary and its options
and **do not run against ccfish today** — ccfish has no NNUE net, no Syzygy
support, and a different bench signature. Do not wire them into `build.sh`
expecting them to pass.

`scripts/` is the same: an upstream mirror, not ccfish-owned.
