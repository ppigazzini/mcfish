# Syzygy tablebases

Perfect endgame play from precomputed tables: WDL (win/draw/loss) for the search,
DTZ (distance-to-zeroing) for the root. mcfish probes both.

Audience: engine and platform contributors. The prober lives in `src/platform/`
but the integration lives in `src/engine/search/`, which is why this page spans
both zones rather than sitting inside [06-platform.md](06-platform.md).

**Wired, and gated.** All six files under
[`src/platform/syzygy/`](../src/platform/syzygy) plus
[`tablebase.c`](../src/platform/tablebase.c) are in **both** `SOURCES` and
`ENGINE_SOURCES`, the four UCI options are live, and `./build.sh tb` compares
discovery and the root probe against `tools/tb.golden`. This is not one of the
zone's unwired rows — read [06-platform.md](06-platform.md) for those.

**Tables are a runtime input, like the net**, and live beside it in
`resources/syzygy/`. With no `SyzygyPath` the max cardinality is 0, the root
ranking never runs and the in-search probe never fires. That is the state `bench`
runs in, and it is why wiring the prober left the signature untouched.

## Modules

| Module | Owns |
| --- | --- |
| [`tablebase.c`](../src/platform/tablebase.c) | the facade — the one surface the engine and shell call; every function delegates in one line |
| [`syzygy/encode.c`](../src/platform/syzygy/encode.c) | pure board geometry: binomials, the king-pair map, the leading-pawn encoding |
| [`syzygy/tables.c`](../src/platform/syzygy/tables.c) | the on-disk data model: `PairsData`, `LR`, `SparseEntry`, the unaligned/endian readers |
| [`syzygy/decode.c`](../src/platform/syzygy/decode.c) | the compressed stream: `decode_set_sizes` and the hot `decode_pairs` |
| [`syzygy/registry.c`](../src/platform/syzygy/registry.c) | the material-key→table map, the lazy mmap and the file parse |
| [`syzygy/wdl.c`](../src/platform/syzygy/wdl.c) | the WDL probe, including the capture recursion upstream calls `search` |
| [`syzygy/probe.c`](../src/platform/syzygy/probe.c) | the DTZ probe and the two public entry points |
| [`../src/engine/search/tb_source.h`](../src/engine/search/tb_source.h) | the seam: three function pointers the engine reads, so `engine/` never includes `platform/` |
| [`../src/engine/search/root_move_build.c`](../src/engine/search/root_move_build.c) | root-move ranking by DTZ, then by WDL |
| [`../src/engine/search/syzygy_pv.c`](../src/engine/search/syzygy_pv.c) | extending a tablebase-scored PV toward mate |
| [`../src/shell/syzygy_option.c`](../src/shell/syzygy_option.c) | the four options, and the install that binds every seam |

Goldens are named in each file's header. The core is upstream
`syzygy/tbprobe.cpp`: `do_probe_table` at `:772`, `probe_table` at `:1305`,
`search` at `:1332`, `probe_dtz` at `:1601`, `rank_root_moves` at `:1780`,
`Tablebases::init` at `:1397`.

## What the files are

A table is a mapped file whose headers and indices are **little-endian** and
whose compressed blocks are **big-endian**, on every host, and whose bytes are
unaligned. Every multi-byte read goes through the `rd_*` helpers in
[`tables.h`](../src/platform/syzygy/tables.h); casting a mapped pointer to a
wider type is never correct here. `LR` carries a `static_assert(sizeof(LR) == 3)`
because it overlays the file directly.

The split between the four platform files is deliberate:

- **`encode.c` has no I/O and no engine types.** Every table reads as zero until
  `encode_init_geometry` runs, so `registry_init` builds the geometry before it
  registers a table.
- **`decode_set_sizes` is the only place a table's shape is validated.** It
  bounds-checks every field against the mapped length and refuses an inverted or
  oversized symbol-length pair. That is what lets `decode_pairs` — the hot path —
  walk the stream carrying only the guards a corrupt file makes unavoidable.
- **`registry.c` is imported by `wdl.c` and `probe.c` and never the reverse**, so
  neither side becomes a god-file.

## Loading

`tablebase_init` scans a `SyzygyPath` and is the only way tables enter the
process. It may be called again for a new path and releases the previous set:
**a `TBTable`'s mapped pointers stay valid only until the next `registry_init`**,
which unmaps every file and frees every arena chunk at once.

Discovery walks the colon-separated path list, builds each material stem
(`KQvK`, `KRvK`, …) and registers a table when its `.rtbw` exists. The `.rtbz` is
counted but does not decide existence. Files are **not** read at discovery time —
only their presence is checked.

**A corrupt file does not kill the process.** Upstream prints `Corrupt tablebase
file` and `exit()`s (`syzygy/tbprobe.cpp:267-271`); mcfish prints the same
diagnostic and reports that file unavailable, so one bad table does not take a
GUI's engine down mid-game — the same fail-soft choice mcfish makes for a net
that will not load. **Keep the diagnostic:** without it a corrupt table is
indistinguishable from an absent one, and the engine silently stops probing with
nothing to explain it.

### Concurrency

`registry_init` is **not** thread-safe and is not called concurrently: upstream
runs it from the `SyzygyPath` callback, off the search, and mcfish does the same
from `syzygy_option.c`. Nothing in the code enforces that no search is running
when it fires; the contract is documented, not checked.

The two lazy maps **are** thread-safe, because every probing thread reaches them
— upstream says so at `tbprobe.cpp:1266`. `registry_map_wdl` and
`registry_map_dtz` are double-checked locking: a lock-free `atomic_bool_load` of
`ready`, then a mutex, then a recheck, then the map and parse, and only then the
store that publishes the flag.

**The flag is published LAST.** It used to be raised on entry, before the file
was even opened, so a second thread taking the fast path read either a null base
— reporting "no such table" for a table that exists — or a base whose
`PairsData` was still being parsed underneath it.

WDL and DTZ take **separate** mutexes, because upstream's `static std::mutex`
sits inside a function template and is therefore per instantiation; one lock
would let a `.rtbz` map block an unrelated `.rtbw`. mcfish's `AtomicBool` is
seq_cst where upstream is acquire/release — strictly stronger, so upstream's
guarantee holds, at the cost of a fence on a path taken once per table per game.

## Probing

### The WDL probe

`search_wdl` is what a WDL probe actually *is*: the stored value is wrong for a
position whose every legal move zeroes the fifty-move counter, so the probe
recurses over captures (and, under `check_zeroing`, pawn moves) and compares.
It does and undoes moves on the position it is given and **restores it exactly**,
so the caller may hand it the live search position. Its `StateInfo` is a function
local, as upstream's stack local at `tbprobe.cpp:1335` is — not shared state.

### In-search: Step 6

[`search_main.c`](../src/engine/search/search_main.c) probes at a non-root,
non-excluded node when the position is small enough, the rule50 counter is zero
and there are no castling rights. The gate is `tb_config.cardinality`, which is 0
without a path, so a default build never enters here.

On a hit the value is mapped into the `VALUE_TB` range, offset by ply, and either
cuts with a TT store at `depth + 6` or — at a PV node — raises `alpha` / caps the
value without cutting.

### Root ranking

`root_moves_build` short-circuits on zero cardinality, then hands the root move
list to `tb_rank_moves`, which ranks by DTZ and falls back to WDL. Two details
that are upstream's and easy to get wrong:

- the move is **undone before** the bail-out test (`tbprobe.cpp:1713`), not after;
- on success the ranking sets `cardinality = 0` when DTZ was available or the
  best score is not a win, which is what **disables the in-search Step 6 probe**
  once the root is already resolved.

The root path reaches the tables by serialising the position to FEN and calling
`TbProbeFen`, not through the live-position seam — the ranking replays each root
move on a scratch board.

### The seam

`engine/` must not include `platform/`, so
[`tb_source.h`](../src/engine/search/tb_source.h) declares three function
pointers — `TbMaxCardinality`, `TbProbeFen`, `TbProbeWdlPos` — that
`syzygy_option_install` binds to the facade. Unregistered they answer "no
tablebase", which is a correct engine, not a broken one.

`TbProbeResult` is defined **once**, in `tb_source.h`, and `probe.h` includes it
rather than declaring a structurally identical twin: two copies compile fine
until one is reordered.

## The options

Four, all live, defaults from upstream `engine.cpp:125-134`:

| Option | Default | Range |
| --- | --- | --- |
| `SyzygyPath` | *(empty)* | string, colon-separated |
| `SyzygyProbeDepth` | 1 | 1..100 |
| `Syzygy50MoveRule` | true | check |
| `SyzygyProbeLimit` | 7 | 0..7 |

[`syzygy_option.c`](../src/shell/syzygy_option.c) owns the authoritative values;
`uci.c` registers them in the option map for the handshake and dispatches every
set through one callback. A spin outside its range is **refused, not clamped** —
clamping would silently turn a typo into a different search.

## Extending the reported PV

A tablebase score with a one-move PV is useless to a user, so
[`syzygy_pv.c`](../src/engine/search/syzygy_pv.c) walks the line out. Two loops:

1. **Truncate** to what is still validated — follow the existing PV while each
   move keeps the top tablebase rank, and stop where it does not.
2. **Extend** toward mate by repeatedly taking the top-ranked move, ranking ties
   by opponent mobility as upstream does at `search.cpp:2174`.

**The PV length and the number of moves made are the same counter.** Let them
drift and the undo walk either leaves a move on the board or unmakes one that was
never made — which is why a draw detected inside a won line is undone *before*
the break, keeping both in step.

The deadline is half of `Move Overhead`, and only under time management. The
warning is taken from **one final reading of the clock, after the undo walk**, as
upstream does at `search.cpp:2223` — not from the loop breaks. A walk that never
trips a break can still finish over budget, and upstream warns there too: the
condition is "the extension ran out of time", not "a loop stopped because of
time".

## Testing

`./build.sh tb` runs two halves and reports them separately:

- **discovery** — the `Found N WDL and N DTZ …` line with no path and with one;
- **the root probe** — `go depth 12` over `tools/cases/tb.fens`, scraping the
  **`info depth 1` line only**.

Depth 1 is the whole point: there the PV is entirely the work of
`syzygy_extend_pv`, because the search has contributed one move and everything
after it is the tablebase's own minimum-DTZ walk. An unported or half-ported
extension shows up as a one-move PV. Score, tbhits and pv are pinned; nodes,
seldepth and bestmove are not — they are search-side, and this is not a search
gate.

**A missing table reads as UNEXERCISED, never as a pass.** Without the full
10-file set the gate checks discovery only and says so in red. `./build.sh
tb-fetch` gets the 3-man set into `resources/syzygy/` and verifies each file's
4-byte magic rather than trusting the HTTP status — a mirror answering a missing
file with a 200 and an HTML error page would otherwise be stored as a table and
fail much later, inside the decoder, as a corrupt-file report. The tables are
never committed.

`tools/tb.golden` is re-derived by `./build.sh tb-update`, which runs **the
oracle** and refuses without the full set. There is no mcfish-derived path to
that golden at all — see [`../tools/GOLDEN_PROVENANCE.md`](../tools/GOLDEN_PROVENANCE.md).

## Gaps

- **The `tb` gate is 3-man only.** The cursed-win / blessed-loss branches of
  `map_score_dtz` and `probe_dtz` need DTZ > 100, reachable only from 5-man
  tables, and are unexercised.
- **No `d`-command probe lines.** Upstream prints `Tablebases WDL:` /
  `Tablebases DTZ:`; mcfish has no such inspection surface, so there is no
  per-position probe output to gate.
- **The material key is local.** Upstream looks tables up by
  `Position::material_key`; mcfish's `Position` carries none, so `registry.c`
  hashes the piece counts with a private fixed-seed table. Only self-consistency
  matters today, because the key never leaves the module — but the fix is to add
  `Key material_key` to `StateInfo`, maintained incrementally by `pos_do_move`
  exactly as upstream does.
- **`gives_check` is passed `false` at both probe call sites.** `pos_do_move`
  ignores the parameter today and recomputes the checkers from the board. The day
  it starts trusting the argument, both call sites must pass a real
  `search_gives_check` or the prober will read a wrong checkers set and mis-probe.
  Both sites carry that warning inline.
