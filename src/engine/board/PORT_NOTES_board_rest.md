# Board zone port notes — the position.c split-out

Integration spec for the eight new modules that carve FEN, Zobrist, legality, the
derived queries, the snapshot and the StateInfo arena out of `position.c`. Nothing
outside them was edited; every change below is still to be applied.

New files:

```
src/engine/board/position_types.h      (header only)
src/engine/board/zobrist.h/.c
src/engine/board/fen.h/.c
src/engine/board/fen_parse.h/.c
src/engine/board/legality.h/.c
src/engine/board/position_query.h/.c
src/engine/board/position_snapshot.h/.c
src/engine/board/state_list.h/.c
```

## 1. Build wiring

Add to `SOURCES` **and** `ENGINE_SOURCES` in `build.sh`:

```
src/engine/board/zobrist.c
src/engine/board/fen.c
src/engine/board/fen_parse.c
src/engine/board/legality.c
src/engine/board/position_query.c
src/engine/board/position_snapshot.c
src/engine/board/state_list.c
```

**Do not add them before §2 is applied.** Five symbols are defined both here and in
`position.c` and will collide at link:

| symbol | new home | delete from |
|---|---|---|
| `pos_fen`, `pos_pretty` | `fen.c` | `position.c` |
| `pos_set` | `fen_parse.c` | `position.c` |
| `pos_legal`, `pos_pseudo_legal` | `legality.c` | `position.c` |
| `see_ge` | `legality.c` | `search/movepick.c` |

## 2. What `position.c` gives up

### 2.1 The Zobrist tables

Delete `Zobrist_psq` / `Zobrist_enpassant` / `Zobrist_castling` / `Zobrist_side` /
`Zobrist_no_pawns`, `next_key` and the body of `position_init`, and
`#include "zobrist.h"`. The five tables keep their names and are now `extern`, so
no call site in `position.c` changes.

`position_init` becomes the sequencing point, not the generator:

```c
void position_init(void) {
    zobrist_init();
    repetition_init(Zobrist_psq, Zobrist_side);
}
```

**The draw order is the anchor.** `zobrist_init` reproduces `position_init`'s
sequence byte for byte: 14 * 64 psq draws for `pc = W_PAWN..B_KING` — *including*
the two unused encoding gaps 7 and 8 — then 8 en-passant, 16 castling, side,
no-pawns. Verified: the generated tables are identical to the current
`position_init`'s, and `pos_set` on the start position yields
`C82F0503486429DD`, the key `tools/board.golden` and `tools/errors.golden` pin.

mcfish's existing sequence is the one in force and the one reproduced. Any other
draw order — skipping pieces 7 and 8, or zeroing the pawn rows on the promotion
ranks — shifts every key from `B_PAWN` onward, so changing it is a separate,
deliberate, signature-moving commit.

### 2.2 FEN

Delete `pos_set`, `pos_fen`, `pos_pretty` and their `position.h` declarations;
`fen.h` and `fen_parse.h` declare them. `<stdio.h>` and `strchr` go with them.

`fen_parse.c` carries **static copies** of `put_piece`, `slider_blockers`,
`set_check_info`, `compute_key` and `set_castling_right`, because `position.c`'s
are `static` and parsing runs before any StateInfo chain exists. That duplication
is deliberate and temporary — when `move_do` and `state_setup` are split out,
both callers reduce to one definition.
Until then, **`compute_key` and `set_check_info` exist in two files and must be
edited in lockstep.** `material_key` was added to `position.c`'s `compute_key`
while this work was in flight and has already been mirrored across; treat that as
the pattern.

### 2.3 Legality

Delete `pos_legal` and `pos_pseudo_legal` from `position.c` and `see_ge` (plus its
`least_significant_bb` helper) from `search/movepick.c`; `legality.h` declares all
three. `movepick.h`'s `see_ge` declaration should be dropped in favour of
`#include "../board/legality.h"`. `search.c`'s local `static bool pseudo_legal` is
the same function under the name PORT_NOTES_search.md §1 asked for and can be
deleted with the same include.

Verified against the copies being replaced: `pos_pseudo_legal` agrees with
`search.c`'s over all 65 536 move words on four positions; `see_ge` agrees with
`movepick.c`'s over 5 638 128 (move, threshold) pairs; `perft` matches the
reference counts with `generate_legal` driven by `legality.c`'s `pos_legal`.

`legality.c` still calls `pos_attackers_to_occ`, which stays in `position.c`. When
`state_setup` lands, that function moves with it and `legality.c`'s include
narrows.

### 2.4 Nothing else moves

`pos_do_move` / `pos_undo_move` / the null-move pair / `pos_attackers_to_occ` /
`pos_non_pawn_material` / `set_check_info` stay in `position.c`. This split is the
read-only and setup halves only.

## 3. `position_types.h` — the shim, and how it stops being one

`position.h` currently DEFINES `StateInfo` and `Position`, so `position_types.h`
forwards to it rather than defining them twice. Every new module includes
`position_types.h`, never `position.h` directly.

To finish the split: move the two `typedef struct { ... }` blocks verbatim out of
`position.h` into `position_types.h`, and have `position.h` `#include
"position_types.h"`. **No include site changes** — that is the point of routing
through the shim now. `state_list.c` in particular then depends only on the type,
not on the mover.

## 4. `position_snapshot` — one field is missing, and why

`PositionSnapshot` is missing `castling_impeded[16]`. Upstream computes it as
`pieces() & castling_path[cr]`,
and mcfish's `Position` has **no `castling_path`** — `set_castling_right` records
the rook square but not the path. Add the field, and this line to
`pos_fill_snapshot`, when Chess960 castling paths land:

```c
    for (int cr = 1; cr <= 8; cr <<= 1)
        out->castling_impeded[cr] = (pos->by_type[ALL_PIECES] & pos->castling_path[cr]) != 0;
```

`position_snapshot.c` calls `position_query` and `legality` directly: C has no
import cycle to break, so there are no function-pointer hooks to register.

`material_value` uses `pos_non_pawn_material`, recomputed from piece counts. Swap
to an incrementally maintained `st->non_pawn_material[c]` when `StateInfo` gains
the field; the value is the same either way.

## 5. `state_list` — no caller yet

`state_list.c` compiles and passes ASan/UBSan but has no caller: the UCI layer
still holds its StateInfo chain some other way. Wire `shell/`'s `position` command
onto `pending_states_create` / `_reset` / `_push` / `_move_out` when the UCI zone
is next opened. Pointer stability is the contract the module exists for — read the
header before substituting a flat array.

## 6. Behaviour changes this lands

**None.** Every function is a faithful move of code already in the tree, and the
FEN acceptance set is unchanged: 400 000 fuzzed records (mutated start positions
and random strings over the FEN alphabet) produced zero accept/reject
disagreements with the current `pos_set` and zero differences in `key`,
`material_key`, `pawn_key`, `minor_piece_key`, `non_pawn_key`, `board`,
`castling_rights`, `ep_square`, `rule50`, `game_ply`, `checkers`, `blockers` or
`pinners` on the accepted ones.

Several stricter rejections are deliberately NOT ported here — pawns on the back
ranks, more than 32 pieces, unreachable promotion counts, counter range checks,
and the king-can-be-captured test. Each is a real upstream check
(`Stockfish/src/position.cpp:279-290`) and each changes which inputs reach the
search, so each belongs in its own commit with the golden re-derived. `pos_fen`'s
Chess960 castling output (upstream emits the rook file letter, mcfish emits
`KQkq`) is the same kind of gap and is likewise untouched.

The signature does not move on this commit. If it does, the split was not faithful.
