# Syzygy port notes — what this module needs from files it must not edit

The prober under `src/platform/syzygy/` and its facade `src/platform/tablebase.h`
are self-contained and compile clean today. Three things outside them are needed
before the engine actually probes, and one is a fidelity gap to close later.

## 1. Build wiring (`build.sh`)

Add to `SOURCES` **and** `ENGINE_SOURCES`:

```
src/platform/syzygy/tables.c
src/platform/syzygy/encode.c
src/platform/syzygy/decode.c
src/platform/syzygy/registry.c
src/platform/syzygy/wdl.c
src/platform/syzygy/probe.c
src/platform/tablebase.c
```

Nothing else in the build changes: the module needs only `-D_POSIX_C_SOURCE=200809L`,
which `CFLAGS_COMMON` already sets, and links against no library.

## 2. UCI options (`src/shell/uci.c`)

Upstream defines three, and this module implements the first outright:

| Option | Type | Default | Owner |
| --- | --- | --- | --- |
| `SyzygyPath` | string | `<empty>` | call `tablebase_init(value, len)` on every set |
| `SyzygyProbeDepth` | spin 1..100 | 1 | the search: minimum depth at which to probe in tree |
| `SyzygyProbeLimit` | spin 0..7 | 7 | the search: probe only when `popcount <= min(limit, tablebase_max_cardinality())` |
| `Syzygy50MoveRule` | check | true | root ranking: whether cursed wins count as wins |

`tablebase_init` is safe to call repeatedly; each call releases the previous
mapping set. Call it once at startup with the default (empty) value too, so the
registry starts empty rather than uninitialised — though every entry point already
reports "unavailable" before any call.

Upstream prints `info string Found N tablebases` after init;
`tablebase_found_wdl()` supplies N.

## 3. Search seam (`src/engine/search/search.c`)

Two call sites, matching upstream:

- **In search, before the TT store at step 6.** When
  `popcount(pieces(pos)) <= min(SyzygyProbeLimit, tablebase_max_cardinality())`,
  `depth >= SyzygyProbeDepth`, `rule50 == 0` and the position has no castling
  rights, call `tablebase_probe_wdl_pos(pos)`. `available == 0` means "no result" —
  continue the normal search. It does and undoes moves on the live position and
  restores it exactly; it never touches the accumulator stack. Count a successful
  probe in `tbHits`.
- **At the root.** Upstream's `root_probe` / `root_probe_wdl`
  (`syzygy/tbprobe.cpp:1672` and `:1746`) rank the root moves by DTZ (or WDL) and
  set `rootInTB`. **Neither is ported here, because zfish does not port them
  either** — zfish ranks root moves in `engine/search/root_move_build.zig` through
  its `tb_source.probeFen` seam. The equivalent here is
  `tablebase_probe_fen(fen, len, chess960)`, which returns both WDL and DTZ for one
  position; the ranking loop itself belongs in the search zone and is the one piece
  of upstream `tbprobe.cpp` this module deliberately does not own.

## 4. Fidelity gap: the material key is local

Upstream looks tables up by `Position::material_key`, which ccfish's `Position`
does not carry. `registry.c` therefore hashes `pos->piece_count` with a private
fixed-seed table (`syzygy_material_key`). This is correct — a registered table and
a probed position are hashed by the same function, and the key never leaves the
module — but it costs a hash of 16 counts per probe where upstream reads a field.

**Requested when `Position` is next revisited:** add `Key material_key` to
`StateInfo`, maintained incrementally by `pos_do_move` exactly as upstream does,
and this module will read it instead. That is a `position.h`/`position.c` change
and is not made here.
