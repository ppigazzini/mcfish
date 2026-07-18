# Port notes — history.c / movepick.c

Changes needed in files outside this module. Nothing here has been applied.

## 1. `StateInfo` is missing the four keys the histories are indexed by

`src/engine/board/position.h` — add to `StateInfo`, and maintain them in
`pos_set` / `pos_do_move` / `pos_undo_move` alongside `key`:

```c
Key pawn_key;              // pawns + both kings
Key minor_piece_key;       // knights, bishops + both kings
Key non_pawn_key[COLOR_NB];  // all non-pawn pieces, per color
```

Upstream golden: `Position::pawn_key()`, `minor_piece_key()`,
`non_pawn_key(Color)` in `position.h`; zfish `position_types.zig`
`StateInfo.pawn_key / minor_piece_key / non_pawn_key`.

Until they exist, `history_update_quiet`, `history_update_all_stats` and
`movepick_init` take `Key pawn_key` as an explicit parameter, and
`history_update_correction` takes a `CorrectionKeys` struct. Once the fields
land, those parameters should be dropped and read off `pos->st` instead — that is
the only change needed in this module.

## 2. `see_ge` belongs in the board zone, not here

`movepick.c` defines `see_ge`, mirroring upstream `position.cpp:
Position::see_ge` (zfish `engine/board/legality.zig: seeGe`). It is declared in
`movepick.h` so the search can use it for the SEE pruning schedule.

Requested move: add to `src/engine/board/position.h`

```c
bool pos_see_ge(const Position *pos, Move m, Value threshold);
```

implemented in `position.c`, and delete the copy in `movepick.c`. It reads only
`board`, `by_type`, `by_color`, `st->pinners`, `st->blockers` and
`pos_attackers_to_occ`, all of which already exist.

## 3. `pos_pseudo_legal` does not exist

`movepick_init` returns `tt_move` unchecked at the `*_TT` stage, as upstream
does. Upstream guards the constructor with
`ttm && pos.pseudo_legal(ttm) ? ttm : Move::none()`. Requested:

```c
bool pos_pseudo_legal(const Position *pos, Move m);
```

in `position.h` / `position.c` (upstream `Position::pseudo_legal`, zfish
`legality.zig: pseudoLegal`). Until it exists, the caller must pass MOVE_NONE
whenever the TT move cannot be trusted, or the picker will hand back an illegal
move that `pos_legal` will not necessarily reject.

## 4. What the search stack must carry

`history.h` deliberately does not depend on `search.c`'s `Stack`. The caller
gathers `HistoryStack` per call. For that to be cheap, `search.c`'s stack entry
needs these fields (upstream `Search::Stack`):

- `Move current_move`
- `int16_t *continuation_history` — set from `cont_hist_page(...)`
- `int16_t *continuation_correction_history` — set from `cont_corr_page(...)`
- `bool in_check`, `bool tt_hit`, `int move_count`, `int stat_score`, `int ply`

`HistoryStack.frames[k]` is `(ss - 1 - k)` for k in [0, 7);
`cont_corr[0]` is `(ss - 2)->continuation_correction_history` and `cont_corr[1]`
is `(ss - 4)->continuation_correction_history`.

The stack must be padded by at least 7 sentinel entries below the root, each
with `current_move = MOVE_NONE` and its pointers set to the table bases
(`cont_hist_page(h, false, false, NO_PIECE, SQ_A1)` /
`cont_corr_page(h, NO_PIECE, SQ_A1)`), or the six-ply walk reads off the front.

## 5. `build.sh`

`src/engine/search/history.c` and `src/engine/search/movepick.c` must be added
to both `SOURCES` and `ENGINE_SOURCES`.

## 6. Not ported here

- `correctionValue()` (the weighted blend of the six correction reads) lives in
  zfish `search.zig`, not `history.zig`, so it belongs with the search port.
  `history.h` exposes `corr_bundle()` for it to read through.
- `tt_move_history` is stored and cleared but never updated; its update site is
  in `search.cpp`, not in the ported modules.
- Threading: the pawn/correction tables are sized for one thread
  (`CORRHIST_BASE_SIZE`, `PAWN_HISTORY_BASE_SIZE` x 1) and live inside
  `Histories`. M4 must re-split them into a NUMA-replicated shared block and
  scale both sizes by `next_power_of_two(threads_on_node)`.
