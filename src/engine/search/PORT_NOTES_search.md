# Port notes — search.c (movepick / history / timeman wiring)

Gaps worked around locally. `search.c` is now the facade over the decomposed zone
— see `PORT_NOTES_search_zone.md` — so the items below that survived the swap are
owned there; this file records what each one costs and what closes it.

## 1. `pos_pseudo_legal` is carried in the search zone

`PORT_NOTES_movepick.md` §3 asks for it. `MovePicker` returns the TT move
unchecked, and `pos_legal` is only defined on pseudo-legal input, so a stale or
key-colliding TT entry would reach `pos_do_move`.

The predicate lives in `search_common.c` as `search_pseudo_legal`, ported from
zfish `engine/board/legality.zig: pseudoLegal` against the golden
`Stockfish/src/position.cpp: Position::pseudo_legal`. `search.c`'s own copy is
gone with the monolith.

**`src/engine/board/legality.c` already holds the canonical `pos_pseudo_legal`.**
Delete `search_pseudo_legal` in the commit that puts that module in `SOURCES`.

## 2. No `ucinewgame` hook into the search

Upstream clears the history block and the time manager's carried node budget from
`Search::clear()`, called on `ucinewgame`. `src/shell/uci.c` calls only
`tt_clear()` there, and this task may not edit it.

`search_go` therefore calls `history_clear` on every `go`, and resets the time
manager, `OriginalTimeAdjust` and the three `SearchIdState` manager scalars with
it. That is what keeps `bench` (which clears the TT per position) reproducible and
what `test_search`'s determinism check rests on. It costs a ~30 MB zero-fill per
move and throws away between-move ordering knowledge.

Requested: a `void search_clear(void)` in `search.h`, called from `uci.c`'s
`ucinewgame` branch, doing `history_clear` + `history_fill_low_ply` +
`timeman_clear` and resetting the per-game scalars. Then drop the per-`go` reset.

## 3. `SearchLimits` / UCI options the time manager cannot see

`PORT_NOTES_timeman.md` already records `npmsec` and `start_time`. Two more:

- **"Move Overhead"** is not a UCI option in `src/shell/uci.c`, so
  `TimemanOptions.move_overhead` is hardcoded to upstream's default of 10 ms.
- **"Ponder"** is an option upstream; mcfish only has `go ponder`, so
  `TimemanOptions.ponder` is fed from `SearchLimits.ponder`. These coincide
  during a ponder search and differ when the option is on but the search is not
  a ponder search.

## 4. No tablebase value sentinels

`to_corrected_static_eval` clamps into `[VALUE_TB_LOSS_IN_MAX_PLY + 1,
VALUE_TB_WIN_IN_MAX_PLY - 1]` upstream. `types.h` has no `VALUE_TB*`, so the
clamp uses the mate band (`VALUE_MATED_IN_MAX_PLY + 1 ..
VALUE_MATE_IN_MAX_PLY - 1`). Widen it by exactly `VALUE_MATE_IN_MAX_PLY -
VALUE_TB` once the Syzygy value model lands.

## 5. Upstream's pruning schedule is live

Superseded. The placeholder set this file described — null move, reverse futility
and a depth/move-count LMR — is gone with the monolith. `search_main.c` /
`search_back.c` carry upstream's Steps 1-21: razoring, ProbCut, singular extensions
and `excluded_move`, `tt_pv` / `improving` / `opponent_worsening` / `cut_node` /
`all_node`, the 1024-scaled reduction table, SEE and futility pruning in the move
loop, the root move list with aspiration windows and MultiPV, PV collection, and
`tt_move_history`.

`ss->stat_score` is computed at the make-move site with upstream's
`capture_stat_score` / `quiet_stat_score`, read back one ply up as
`HistoryStack.prev_stat_score`, and now feeds the LMR reduction as upstream's does.
