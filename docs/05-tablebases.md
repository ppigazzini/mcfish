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
discovery and the root probe against `tools/tb.golden`. Nothing in this zone — or
any zone — sits outside the source arrays today; see
[00-architecture.md](00-architecture.md) for the check that establishes it.

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

**Every refusal says so, not just the cheap one.** That rule was stated here and
honoured at ONE of the reader's refusal sites — the length-shape check — while a
bad magic and every refusal `set()` makes returned in silence, which is the case a
crafted file actually reaches. All three report now, and the message names the
file that was OPENED, so a table that is simply absent stays silent. The buffer
carrying that name lives in a `noinline` helper: its callers open with the `ready`
early return taken on every probe, and a frame sized for the refusal cost 0.13% of
a probing search when it sat there.

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

**The flag is published LAST**, after the map and the parse. Raise it any earlier
— on entry, before the file is even opened — and a second thread taking the fast
path reads either a null base, reporting "no such table" for a table that exists,
or a base whose `PairsData` is still being parsed underneath it.

WDL and DTZ take **separate** mutexes, because upstream's `static std::mutex`
sits inside a function template and is therefore per instantiation; one lock
would let a `.rtbz` map block an unrelated `.rtbw`. mcfish's `AtomicBool` is
seq_cst where upstream is acquire/release — strictly stronger, so upstream's
guarantee holds, at the cost of a fence on a path taken once per table per game.

## The compressed format, as implemented

`registry_init`'s parse fills one `PairsData` per `(side, file)`, and what it
fills is exactly the layers [`decode.c`](../src/platform/syzygy/decode.c) and
[`tables.h`](../src/platform/syzygy/tables.h) implement:

| Layer | What the code does |
| --- | --- |
| Symbols / btree | `LR` (`tables.h`) is a 3-byte entry packing two 12-bit symbols — `lr_left`/`lr_right` unpack them. `lr_right(e) == 0xFFF` marks a leaf, whose `lr_left(e)` is the stored value. Golden: upstream `SparseEntry` `tbprobe.cpp:192`, `LR` `:201`. |
| Symbol lengths | `set_sym_len`, called from `decode_set_sizes` for every unvisited symbol, fills `d->symlen` by recursive descent over the btree: a leaf is 0, an internal symbol is `symlen[left] + symlen[right] + 1` — the count of values that symbol represents, minus one. Golden: `set_symlen` `tbprobe.cpp:1061`. |
| Canonical Huffman | `decode_set_sizes` builds `d->base64` from `d->lowest_sym`, right-padded so `base64[i] >= base64[i+1]`, and records `min_sym_len`/`max_sym_len`. Golden: `set_sizes` `tbprobe.cpp:1080-1137`, the `base64` comment at `:366`. |
| Per-length tables | Three of the values `decode_pairs` formed per SYMBOL depend only on the symbol's LENGTH, of which a table has at most 63: the right-padding shift, the lowest symbol of that length, and the real bit length consumed. `decode_set_sizes` fills `len_shift`/`len_offset`/`len_real` once per table, turning five arithmetic operations and a read out of the mapping into three loads, and the subtraction folds in exactly — `base64[len]`'s low `shift` bits are zero and the scan only stops where `buf64 >= base64[len]`, so there is no borrow to lose. The per-symbol shift RANGE test went with them: the refusal above bounds it to `[1, 63]` for every length the scan can return, so it could not fire. |
| Indices | `SparseEntry` (`tables.h`) is 6 bytes (`block[4]`, `offset[2]`). `sparse_index_size` and `block_length_size` are computed in the registry parse from the table's `span`/`blocks_num` (`registry.c`). |
| Pairs data | `decode_pairs` locates the block through the sparse index, walks `block_length[]` to the exact block, reads that block's bitstream in **big-endian** 64-bit windows, decodes the symbol against `base64`, then descends the `LR` btree to the leaf value. Golden: `decompress_pairs` `tbprobe.cpp:602`. |
| Single value | When `TB_FLAG_SINGLE_VALUE` is set, the table stores one value and `decode_pairs` returns it for every index without touching the bitstream at all. |

The six [`decode.h`](../src/platform/syzygy/decode.h) flags —
`TB_FLAG_STM`, `TB_FLAG_MAPPED`, `TB_FLAG_WIN_PLIES`, `TB_FLAG_LOSS_PLIES`,
`TB_FLAG_WIDE`, `TB_FLAG_SINGLE_VALUE` — gate the handful of format variants
`decode.c` and [`wdl.c`](../src/platform/syzygy/wdl.c) branch on: `TB_FLAG_WIDE`
doubles the remap-table element width, `TB_FLAG_MAPPED` says the raw decoded
value needs a per-WDL-class remap before it means anything.

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

### TB score values

[`score.h`](../src/engine/board/score.h) places the tablebase band directly
below the mate band, an invariant it states as one fact written twice:

| Constant | Value | Meaning |
| --- | --- | --- |
| `VALUE_TB` | `VALUE_MATE_IN_MAX_PLY - 1` | the top of the TB band, one below where a score reads as mate |
| `VALUE_TB_WIN_IN_MAX_PLY` | `VALUE_TB - MAX_PLY` | the threshold above which a score's magnitude reads as tablebase-decisive |
| `VALUE_TB_LOSS_IN_MAX_PLY` | `-VALUE_TB_WIN_IN_MAX_PLY` | the loss-side mirror |
| `MAX_DTZ` | `1 << 18` (`root_move_build.c`) | the root-ranking scale `WdlToRank`/`WdlToValue` are built from |

`score_classify` (`score.h`/`score.c`) is the pure classifier every UCI-facing
score passes through: `SCORE_NON_DECISIVE` below `VALUE_TB_WIN_IN_MAX_PLY`,
`SCORE_TABLEBASE` between there and `VALUE_TB` (carrying the signed distance to
the outcome and which side wins), `SCORE_MATE` above it. `search_emit.c` calls
it with these four thresholds live, never re-derived — the classifier takes
them as arguments precisely so it stays a pure function nothing can drift from
the search's own definitions.

### UCI reporting

Two things a tablebase result changes about what `go` prints, both in
[`search_emit.c`](../src/engine/search/search_emit.c):

- **The score.** `uci_format_score`
  ([`uci_wdl.c`](../src/engine/search/uci_wdl.c)) renders `SCORE_TABLEBASE` as
  `cp ±20000 - value` — a large but non-mate centipawn score, upstream's own
  convention for "decisive but not a forced mate the PV proves out." When the
  root move came from the tablebase ranking (`tb_config.root_in_tb`) and the
  score is not a genuine mate, `search_emit.c` substitutes the root move's
  `tb_score` for the searched value before formatting it, and reports that
  substituted score as an exact bound.
- **`tbhits`.** Reported as the pool's summed in-search hits
  (`pool_tb_hits`, through the same `PoolCounters` seam
  [04-multithreading.md](04-multithreading.md) describes) **plus**
  `root_moves_count` whenever `root_in_tb` — the root-ranking probes count
  too, not only the in-tree Step 6 ones.

`d`'s `Tablebases WDL:`/`DTZ:` lines are the shell's, not this zone's — see
[07-shell.md](07-shell.md).

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

Both of those gates drive **well-formed** tables, which is the wrong shape for
the question the parse actually has to answer. A table is untrusted input:
`SyzygyPath` names a file the engine did not write, and every offset the parse
advances is a value read out of that same file. `./build.sh fuzz-tb` is the gate
for that half, in two lanes. One calls `decode_set_sizes` and `decode_pairs`
directly, carving the file-backed regions exactly as `set` does so a reported
crash is one a real `.rtbw` could cause; it is fast enough to explore header
shapes, and it is the lane that found the three decoder bugs. The other writes
real files, points a real `SyzygyPath` at them, probes, and then ranks the root —
a thousand times slower, and the only one that runs `set`, `set_groups`,
`set_dtz_map` and `map_file` rather than a model of them. It is seeded from the
3-man set when `./build.sh tb-fetch` has been run, so mutation starts from a
table that parses.

**The probe is not the only consumer of a score the file decided.** The
whole-file lane stopped at `tablebase_probe_fen` — the value — while the ROOT
RANKING is what indexes with it, through the five-entry `WdlToRank[wdl + 2]` and
`WdlToValue[]` in `root_move_build.c`. That surface belonged to no tablebase
lane, and an unbounded score reached it as a negative index. The lane now drives
`root_moves_build` over the same position for the same reason it reaches the
probe through `tablebase_init` rather than through `set`: it is the engine's own
entry point, and a corrupt file reaches it on every `go`. Extend the lane
whenever a new consumer reads a probe's answer.

Treat the bounds in `decode.c`, `registry.c` and `wdl.c` as load bearing: the
three bugs that gate found on its first run had survived a hand-written bounds
pass that reads as complete, and the fourth — the WDL score itself, which
`wdl.h` had promised was in -2..2 and nothing enforced — survived because no
lane drove the code that indexed with it.

**Fuzzing asks whether a bound can be broken; it does not ask whether one that
held yesterday still holds.** `./build.sh malformed` is the regression half, and
it runs in `parity` because it costs 2.4 s. Two families, and the second is the
stronger:

- **REFUSED** — five crafted 80-byte headers, each wrong in one field. They prove
  a crafted header is refused SAFELY and says so: exit 0, no sanitizer report, a
  diagnostic naming the file, and the engine still answers. What they cannot
  prove is WHICH check fired, because the reader emits one message for every
  refusal — so their names were re-derived against an instrumented build rather
  than trusted, and two inherited from a sibling were dropped for gating nothing
  here.
- **ABSORBED** — four real 3-man tables with a handful of bytes changed, replayed
  as byte lists rather than fuzz seeds. These LOAD, so the search reaches the
  decode loop, which is where the per-symbol bounds live and where no crafted
  header can reach: an 80-byte file is refused long before, its sparse index alone
  outrunning the file. Their judge asks for SURVIVAL, not a diagnostic — these land
  on fields whose only constraint is internal consistency, and the format records
  nothing saying which value was meant, so a reader cannot detect them and must
  not pretend to.

Both families are held by `negative-control` rows, and the second row is what says
the absorbed family reaches the decoder: remove `sym >= symlen_size` and
`symbol-past-end` becomes an ASan heap-buffer-overflow.

## Gaps

- **The `tb` gate is 3-man only** — but the cursed-win / blessed-loss branches of
  `map_score_dtz` and `probe_dtz` are no longer unexercised. They need DTZ > 100,
  so only a 5-man table reaches them, and `./build.sh tb-cursed` drives exactly
  that against `tools/tb_cursed.golden` after `./build.sh tb-fetch 5`. It is
  deliberately outside `parity`, because it depends on tables `tb-fetch` does not
  get by default and a gate that is usually skipped stops being read. Run it by
  hand when touching the prober; it exits **127**, not 0, when the tables are
  absent. Its golden mixes provenance — oracle-pinned probe results over two
  self-golden node totals — and `./build.sh tb-cursed-update` re-derives only the
  latter, refusing outright if the former has moved. See
  [10-tooling-ci.md](10-tooling-ci.md).
- **The material key is local.** Upstream looks tables up by
  `Position::material_key`; mcfish's `Position` carries none, so `registry.c`
  hashes the piece counts with a private fixed-seed table. Only self-consistency
  matters today, because the key never leaves the module — but the fix is to add
  `Key material_key` to `StateInfo`, maintained incrementally by `pos_do_move`
  exactly as upstream does.
**Not a gap, and listed here because it reads like one: `gives_check` is a
contract the prober must honour, not a parameter it may ignore.** `pos_do_move`
**trusts** the argument — `new_st->checkers` is derived from it rather than
recomputed from the board (`position.c`) — so every probe site passes the real
`pos_gives_check(pos, m)`: `probe.c` and `wdl.c` on the walk, and both
`pos_do_move` calls in `root_move_build.c` on the root ranking. Passing `false`
there is not a cheap approximation; it hands the child an empty checkers set and
the prober mis-probes with no diagnostic. Each site carries that note inline, and
upstream reaches the same place differently: its `search<CheckZeroingMoves>` uses
the two-argument `do_move`, which computes the predicate itself.
