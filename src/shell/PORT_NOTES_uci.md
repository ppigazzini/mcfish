# UCI shell decomposition port notes

The seven modules under `src/shell/` — `uci_strings`, `uci_format`, `uci_output`,
`uci_input`, `uci_parse`, `uci_bench`, `bench_positions` — are the decomposition
of `uci.c`'s monolithic loop. They compile and are exercised standalone, but
nothing calls them yet: `uci.c` still holds its own copies of the parse and print
paths. Record here what the rewiring commit must decide deliberately.

## Shared-header changes requested (none applied)

None of these blocks compilation; each removes a workaround.

- **`engine.h` — an FEN accessor.** `uci_bench.c` reads the live position's FEN
  through `engine_get_position()` + `pos_fen()`, because there is no
  `engine_fen()`. Upstream's bench calls `engine.fen()` (uci.cpp:266) and zfish
  calls `engine_mod.fenEngine`. An `void engine_fen(char *buf, int buf_len)`
  would let `uci_bench.c` stop including `position.h` at all, which is the only
  reason it does.
- **`engine.h` — `engine_wait_for_search_finished()`.** Upstream's bench waits
  after each `go` (uci.cpp:275). The wait is a no-op while the search is
  single-threaded and synchronous, so `uci_bench.c` omits the call rather than
  inventing a name. It becomes required at M4.
- **`search.h` — publish the node count.** `uci_bench_run` reads each `go`'s
  nodes from `uci_output_last_nodes_searched()`, which nothing sets today. The
  search driver must call `uci_output_set_last_nodes_searched(nodes)` when it
  completes a search, exactly as upstream's `set_on_update_full` capture does
  (uci.cpp:247). Until it does, `bench` through the dispatcher totals zero.
- **`START_FEN` is spelled twice.** `UCI_START_FEN` (uci_strings.h) and
  `ENGINE_START_FEN` (engine.h) are the same 56 bytes. `uci_parse` cannot include
  `engine.h` without dragging `position.h` into a leaf, so it carries its own.
  Collapsing them means moving the literal into a header both can reach.

## Deliberate divergences from the port source

- **No output mutex.** zfish's `uci_output.zig` serialises `printLine` with a
  mutex so a search thread and the listener cannot tear a line. `build.sh` links
  no pthread and the search is single-threaded, so `uci_output.c` has none. The
  funnel is preserved — every line goes through `uci_output_print_line` — so
  adding the lock at M4 is a one-function change.
- **Bounded input line.** Upstream's `std::getline` and zfish's stitching reader
  are unbounded; `UciInput` holds `UCI_LINE_MAX` (64 KiB) and sets `truncated`
  on overflow, consuming the tail so the next read starts on a line boundary.
  zfish's reader returns `null` on `error.StreamTooLong`, which makes the caller
  dispatch `quit` — the bounded form is strictly better behaved, and 64 KiB is
  far past any reachable `position ... moves` line.
- **Whole-token integer parse.** `is >> limits.depth` stops at the first
  non-digit, so upstream reads `go depth 5abc` as depth 5 plus a junk token.
  `uci_parse` requires the whole token and reports `bad_token`. This is zfish's
  choice and is what makes upstream's own "Invalid argument for 'depth'" path
  reachable at all; it differs from the golden only on malformed input.
- **FEN rejoin without the trailing space.** Upstream accumulates
  `fen += token + " "` (uci.cpp:498), leaving one trailing space.
  `uci_parse_position` joins with single separators and no trailing space. FEN
  parsing ignores it and nothing echoes the assembled string — `d` regenerates
  the record through `pos_fen` — so no golden can see the difference.

## Not ported

- **`benchmark` / speedtest** (`uci_bench.zig:benchmarkRuntime`, upstream
  `uci.cpp:311`). Its report needs the NUMA config string, thread-binding
  information, and the large-pages flag, none of which exist yet; the whole
  command lands with M4. `bench_positions.c` therefore carries only `Defaults`,
  not zfish's `BenchmarkPositions`, which only `benchmark` reads.
- **`bench <fenFile>` from a path.** `uci_bench_setup` accepts `default` and
  `current`; a path would put file I/O and an `exit(1)` inside a module that has
  neither. Upstream reads it in `benchmark.cpp:150`.
- **`dbg_print()`** before the bench summary (uci.cpp:300). zfish's
  `misc.zig:dbgPrint` and its counters are unported; `misc.h` has no equivalent.
- **`uci_critical.zig`.** The formatter is ported —
  `uci_format_critical_error` — but the `current_cmd` global and the
  `exit(1)` belong to the dispatch loop that owns the command being run, so they
  land in the `uci.c` rewrite rather than in a module of their own.

## The goldens

`uci_format_info_full` reproduces upstream's composition exactly, in this order:
`depth seldepth multipv score [bound] [wdl] nodes nps hashfull tbhits time pv`.

It cannot reproduce `tools/search.golden` as that file stands. Those lines are
`info depth N score cp N nodes N hashfull N pv M` — a REDUCED form `search.c`
emits today, with `seldepth`, `multipv`, `nps`, `tbhits` and `time` absent
rather than zero. The two cannot both be true, and upstream's is the one a GUI
parses. Cutting `search.c` over to this formatter is an intended behaviour
change and re-derives `tools/search.golden`; it does not touch
`tools/signature.golden`, which counts nodes and not bytes.
