# Syzygy port notes — what this module needs from files it must not edit

The prober under `src/platform/syzygy/` and its facade `src/platform/tablebase.h`
are wired: all seven files are in `build.sh`'s `SOURCES` **and** `ENGINE_SOURCES`,
`src/shell/syzygy_option.c` owns the four UCI options and binds the seams, and
`./build.sh tb` gates discovery and the root probe against the oracle. The module
links against no library and needs only the `-D_POSIX_C_SOURCE=200809L` that
`CFLAGS_COMMON` already sets.

The search side needed no work: Step 6 lives in
`src/engine/search/search_main.c` and the root DTZ/WDL ranking in
`src/engine/search/root_move_build.c`, both already compiled and both reaching the
prober only through the `tb_source.h` function pointers.

## Deviations and gaps

- **Corrupt-file handling is fail-soft.** Upstream (`syzygy/tbprobe.cpp:267`)
  prints `Corrupt tablebase file` and `exit()`s; `registry.c` prints the same
  diagnostic and reports the file unavailable, so one bad file does not take a
  GUI's engine down mid-game. Keep the diagnostic: without it a corrupt table is
  indistinguishable from an absent one.
- **`pos_do_move`'s `gives_check` argument is passed `false`** here and in
  `probe.c`. It is inert — `set_check_info` recomputes the checkers from the board
  (`position.c:621`) — and every caller in the tree except `search_main.c:164`
  does the same. The day that parameter is read, both syzygy call sites must pass
  `search_gives_check(pos, m)`, as upstream's `search<CheckZeroingMoves>` does via
  the two-argument `do_move`.
- **No `d`-command probe lines.** Upstream prints `Tablebases WDL:` /
  `Tablebases DTZ:`; mcfish has no such inspection surface, so there is no
  per-position probe output to gate.
- **The `tb` gate is 3-man only.** The cursed-win / blessed-loss branches of
  `map_score_dtz` and `probe_dtz` need DTZ > 100, reachable only from 5-man
  tables, and are unexercised.

## Fidelity gap: the material key is local

Upstream looks tables up by `Position::material_key`, which mcfish's `Position`
does not carry. `registry.c` therefore hashes `pos->piece_count` with a private
fixed-seed table (`syzygy_material_key`). This is correct — a registered table and
a probed position are hashed by the same function, and the key never leaves the
module — but it costs a hash of 16 counts per probe where upstream reads a field.

**Requested when `Position` is next revisited:** add `Key material_key` to
`StateInfo`, maintained incrementally by `pos_do_move` exactly as upstream does,
and this module will read it instead. That is a `position.h`/`position.c` change
and is not made here.
