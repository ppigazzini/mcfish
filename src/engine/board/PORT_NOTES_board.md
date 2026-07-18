# Board zone port notes — score / board_props / repetition / threats

Integration spec for the four new modules. Nothing outside them was edited; every
change below is still to be applied.

## 1. Build wiring

Add to `SOURCES` **and** `ENGINE_SOURCES` in `build.sh`:

```
src/engine/board/score.c
src/engine/board/board_props.c
src/engine/board/repetition.c
src/engine/board/threats.c
```

## 2. `threats` — the Position integration

### 2.1 `StateInfo` — no change

Threat state is per-move scratch, not per-ply state. Nothing is added to
`StateInfo` for threats.

### 2.2 `Position` — one new field

`src/engine/board/position.h`, at the end of `struct Position` (after `st`):

```c
    // Hold the per-move threat deltas the NNUE incremental update consumes. Live
    // only between pos_do_move and the accumulator's read of it; pos_undo_move
    // does not restore it.
    DirtyThreats scratch_dts;
```

and `#include "threats.h"` at the top of `position.h`.

`threats.h` includes `position.h`, so this is a cycle. Break it the way upstream
does — `DirtyThreats` is a plain struct with no dependency on `Position`:

- move the `DirtyThreats` struct, the `DIRTY_THREAT_*` offsets and the
  `dirty_threat_*` accessors out of `threats.h` into `types.h`, and
- leave `threats.h` holding only `threats_init`, `ray_pass_bb`,
  `dirty_threats_clear` and `threats_update_piece`.

That is upstream's split exactly (`Stockfish/src/types.h:309-345` owns
`DirtyThreat`/`DirtyThreats`; `position.h` owns `update_piece_threats`). Do it as
part of applying this note — I did not, because `types.h` was off limits.

Upstream also carries `DirtyPiece scratch_dp`. That is the NNUE dirty-piece port,
not this one; add it with that module.

### 2.3 Startup

`threats_init()` builds `RayPassBB` from `attacks_bb`, so it must run **after**
`attacks_init()`. Add the call next to it (`shell/main.c` or wherever
`bitboards_init` / `attacks_init` / `position_init` are sequenced):

```c
bitboards_init();
attacks_init();
threats_init();     // new — needs attacks_bb
position_init();
```

### 2.4 The make/unmake hooks

Upstream threads a `DirtyThreats*` through the four board mutators and calls
`update_piece_threats` on each. Mirror it. In `position.c`, give each mutator a
`DirtyThreats *dts` parameter (`NULL` = do not record) and place the call exactly
where upstream places it — the ordering is what makes `occupied` correct for each
direction. `noRays` is `~0ULL` everywhere except `move_piece`.

Golden: `Stockfish/src/position.h:383-437`. Port source:
`zfish/src/engine/board/move_do.zig:87-127`.

```c
static void put_piece(Position *pos, Piece pc, Square s, DirtyThreats *dts) {
    ... existing body ...
    if (dts)
        threats_update_piece(true, pos, pc, true, s, dts, ~0ULL);   // AFTER
}

static void remove_piece(Position *pos, Square s, DirtyThreats *dts) {
    const Piece pc = pos->board[s];
    if (dts)
        threats_update_piece(true, pos, pc, false, s, dts, ~0ULL);  // BEFORE
    ... existing body ...
}

static void move_piece(Position *pos, Square from, Square to, DirtyThreats *dts) {
    const Piece pc = pos->board[from];
    const Bitboard fromto = square_bb(from) | square_bb(to);
    if (dts)
        threats_update_piece(true, pos, pc, false, from, dts, fromto);  // BEFORE
    ... existing body ...
    if (dts)
        threats_update_piece(true, pos, pc, true, to, dts, fromto);     // AFTER
}

// New. Replace a piece in place (the capture-and-promote square).
static void swap_piece(Position *pos, Square s, Piece pc, DirtyThreats *dts) {
    const Piece old = pos->board[s];
    remove_piece(pos, s, NULL);
    if (dts)
        threats_update_piece(false, pos, old, false, s, dts, ~0ULL);  // compute_ray = false
    put_piece(pos, pc, s, NULL);
    if (dts)
        threats_update_piece(false, pos, pc, true, s, dts, ~0ULL);    // compute_ray = false
}
```

`compute_ray` is `false` only in `swap_piece`: the occupancy is unchanged across
the swap, so no ray can be discovered, and `direct_sliders` is folded into the
incoming list instead.

### 2.5 `pos_do_move` — required restructuring

**The current piece-mutation sequence in `pos_do_move` produces the right final
position but the wrong dirty-threat list, and must be replaced.** The list is
order- and content-sensitive: it is the accumulator's input, not a derived fact.

Today (`position.c:399-443`) the order is: remove captured → `move_piece(from,to)`
→ (promotion) `remove_piece(to)` + `put_piece(promoted,to)`. Upstream never moves
a promoting pawn to `to` first, and never uses `move_piece` on a capture.

Replace with upstream's branch (`Stockfish/src/position.cpp` `do_move`; zfish
`move_do.zig:268-279`):

```c
    // Record the pre-move king square before any piece moves.
    dirty_threats_clear(&pos->scratch_dts);
    pos->scratch_dts.us = us;
    pos->scratch_dts.prev_ksq = king_square(pos, us);
    DirtyThreats *const dts = &pos->scratch_dts;

    ... castling / capture-removal (capture removal uses remove_piece(pos, cap_sq, dts)) ...

    if (mt != CASTLING) {
        const Piece to_pc = (mt == PROMOTION) ? make_piece(us, move_promotion(m)) : pc;

        if (captured != NO_PIECE && mt != EN_PASSANT) {
            remove_piece(pos, from, dts);
            swap_piece(pos, to, to_pc, dts);
        } else if (pc == to_pc) {
            move_piece(pos, from, to, dts);
        } else {
            remove_piece(pos, from, dts);
            put_piece(pos, to_pc, to, dts);
        }
    }
```

The Zobrist updates for the promotion (`Zobrist_psq[pc][to] ^
Zobrist_psq[promoted][to]`) move out of the `else if (mt == PROMOTION)` arm at
`position.c:438` and into the `pawn` block unconditionally on `mt == PROMOTION`,
since the piece is no longer routed through `to` as a pawn first.

`do_castling` (`position.c:355-368`) becomes, for the Do direction only:

```c
    remove_piece(pos, kf, dts);
    remove_piece(pos, rf, dts);
    put_piece(pos, king, kt, dts);
    put_piece(pos, rook, rt, dts);
```

in that exact order (zfish `move_do.zig:168-171`). The Undo direction passes
`NULL` — see below.

At the very end of `pos_do_move`, after `set_check_info`:

```c
    pos->scratch_dts.ksq = king_square(pos, us);
```

### 2.6 `pos_undo_move` — no threat recording

`pos_undo_move` passes `NULL` for every `dts`. The accumulator pops its stack; it
does not replay the deltas backwards. Same for `pos_do_null_move` /
`pos_undo_null_move`, which touch no pieces at all.

## 3. `repetition` — the Position integration

### 3.1 `StateInfo` — one new field

`src/engine/board/position.h`, in `struct StateInfo`:

```c
    // Record the ply distance back to the previous occurrence of this key —
    // negative when that occurrence was itself a repetition, zero when the
    // position is new. pos_do_move is the only writer.
    int repetition;
```

Then define `CCFISH_STATEINFO_HAS_REPETITION` (a `-D` in `build.sh`, or a
`#define` in `position.h` above the `repetition.h` include). **Until it is
defined, `repetition.c` compiles but reads zero and every query answers "never
repeated"** — that is deliberate scaffolding so the module can land before the
field, and it is a behaviour change if shipped.

### 3.2 `pos_do_move` must fill it

At the end of `pos_do_move`, after `set_check_info(pos)` and before the
`scratch_dts.ksq` line (`Stockfish/src/position.cpp:1048-1062`, zfish
`move_do.zig:325-337`):

```c
    new_st->repetition = 0;
    const int rep_end = new_st->rule50 < new_st->plies_from_null ? new_st->rule50
                                                                 : new_st->plies_from_null;
    if (rep_end >= 4) {
        const StateInfo *stp = new_st->previous->previous;
        for (int i = 4; i <= rep_end; i += 2) {
            stp = stp->previous->previous;
            if (stp->key == new_st->key) {
                new_st->repetition = stp->repetition != 0 ? -i : i;
                break;
            }
        }
    }
```

`pos_do_null_move` sets `new_st->repetition = 0` after `set_check_info`
(`Stockfish/src/position.cpp:1401`). The root `StateInfo` needs no new code:
`pos_set` already zeroes the whole record (`position.c:143`).

### 3.3 Delete the inline implementation

`bool pos_is_draw(const Position *, int)` at `position.c:505-534` **must be
deleted** — `repetition.c` defines the same symbol and the two will collide at
link time. Its declaration at `position.h:72` should go too; `repetition.h`
declares it.

The deleted version is not equivalent. It threefold-counts by walking keys on
every call; the port reads the distance `pos_do_move` recorded, and answers
`rep != 0 && rep < ply`. It also drew unconditionally on `rule50 > 99`, where
upstream draws only if the side to move is not checkmated. See §5.

### 3.4 Startup

`repetition_init` takes the Zobrist keys rather than re-deriving them — a second
seeded copy of the PRNG is exactly the drift the cuckoo table cannot survive. At
the end of `position_init` (`position.c:34`):

```c
    repetition_init(Zobrist_psq, Zobrist_side);
```

`Zobrist_psq` is `Key[PIECE_NB][SQUARE_NB]`, which decays to the declared
`const Key (*)[SQUARE_NB]`. It also needs `attacks_bb`, so `attacks_init()` must
already have run — it does, `position_init` is sequenced after it.

## 4. `score` and `board_props`

No Position changes. `score.h` defines `VALUE_TB`, `VALUE_TB_WIN_IN_MAX_PLY` and
`VALUE_TB_LOSS_IN_MAX_PLY`, which upstream keeps in `types.h:161`; fold them into
`types.h` when that file next opens, and drop them from `score.h` then.

## 5. Behaviour changes this lands

Applying the above changes engine behaviour, so re-derive the signature only
after `perft` and the golden gate are green, and say which of these moved it:

1. `pos_is_draw` no longer draws on `rule50 > 99` when the side to move is
   checkmated (upstream `position.cpp:1528`: mate beats the fifty-move rule).
2. Repetition detection switches from a per-call key walk to the recorded
   `st->repetition` distance. `is_repetition` is `rep && rep < ply`, which treats
   a single in-line repetition as a draw and requires a threefold at or before the
   root — the current code approximates this with `++repeats + (i < ply) >= 2`.
3. The repetition walks no longer null-check `previous`. Upstream does not either;
   `min(rule50, plies_from_null)` is the bound that keeps them in the chain, which
   requires `pos_set` to zero `plies_from_null` on the root StateInfo. It does.
4. `pos_do_move`'s piece-mutation order changes (§2.5). The final position is
   unchanged; only the dirty-threat list and the promotion key ordering differ.
