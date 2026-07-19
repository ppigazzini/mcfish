# What the NNUE accumulator needs from `Position`

The accumulator in `nnue_accumulator.{h,c}` is **incremental**: it holds one slot per ply,
and each slot carries the accumulator *and the diff that produced it*. It cannot be
wired up until the board zone records that diff. This file states exactly what has to
change and why, so whoever owns `position.{h,c}` can land it in one commit.

Nothing here has been added to the board zone by this port — these are requests.

Upstream golden: `position.h` `DirtyPiece` / `DirtyThreats`.

## 1. Two new records in the board zone

```c
// The HalfKAv2_hm delta of one move.
typedef struct DirtyPiece {
    uint8_t pc;         // the moving piece, NO_PIECE when the move moves nothing
    uint8_t from;       // the square it left
    uint8_t to;         // the square it landed on, or 64 for "none"
    uint8_t remove_sq;  // a captured piece's square, or 64
    uint8_t add_sq;     // a promoted piece's square, or 64
    uint8_t remove_pc;  // the captured piece
    uint8_t add_pc;     // the promoted piece
} DirtyPiece;

// The full_threats delta of one move.
typedef struct DirtyThreats {
    uint32_t list_values[96];
    size_t   list_size;
    uint8_t  us;         // the side that moved
    uint8_t  prev_ksq;   // its king square before the move
    uint8_t  ksq;        // its king square after the move
} DirtyThreats;
```

**The layouts are contractual, not incidental.** `NnueDirtyPiece` and `NnueDirtyThreats`
in `nnue_feature.h` / `nnue_accumulator.h` are byte-identical views of these two, because
the accumulator hands out pointers *into its own arena* for the make-move to write
through. In particular:

- `DirtyPiece` must stay all-`uint8_t` and alignment-free. The arena stores it at an
  offset that is deliberately **not** rounded up, and the refresh test reads `pc` as the
  first diff byte. `nnue_feature.h` static-asserts both facts on its side.
- `us`, `prev_ksq`, `ksq` must stay in that order, contiguous, immediately after
  `list_size`. `nnue_accumulator.c` static-asserts their offsets on its side.
- `64` is "no square" (`NNUE_SQ_NONE`), not `SQ_NONE`'s current value. Squares here are
  raw `uint8_t` in upstream's NNUE encoding, and pieces are `color << 3 | type` — which is
  already what `types.h` `Piece` is, so a cast is enough.

The `list_values` bit layout, written by the board zone and decoded by
`nnue_full_append_changed`:

| bits | meaning |
|---|---|
| 31 | 1 = the move ADDS this threat, 0 = removes it |
| 23..20 | the attacking piece |
| 19..16 | the attacked piece |
| 15..8 | the attacked square |
| 7..0 | the attacking square |

## 2. `pos_do_move` must take and fill both

```c
void pos_do_move(Position *pos, Move m, StateInfo *new_st, bool gives_check,
                 DirtyPiece *dp, DirtyThreats *dts);
```

The two out-parameters are the ones `nnue_acc_stack_push` returns, so the make-move
writes its delta straight into the accumulator's arena and nothing is copied.

`pos_do_null_move` must fill them too: `dp` all-`NO_PIECE`/`64` and `dts->list_size = 0`,
so the incremental step is a no-op rather than reading a stale ply's delta.

## 3. Threat recording inside make/unmake

`DirtyThreats` is filled by a port of upstream `Position::update_piece_threats` and the
slider walk it drives. It runs on every put-piece and remove-piece and appends to
`list_values`. That module is **board-zone work, not eval-zone work**, and it is not
part of this port. It needs `BetweenBB`-style ray queries, which `attacks.h` already has.

The filters it applies are not an optimisation: the (attacker, attacked) pairs it rejects
are exactly the pairs `nnue_full_make_index` maps out of range and the accumulator then
discards, so recording them would be pure work.

## 4. Push and pop at every node

The search must bracket every move:

```c
NnueStackPushOutput out = nnue_acc_stack_push(stack);
pos_do_move(pos, m, &st, gives_check, out.dirty_piece, out.dirty_threats);
...
pos_undo_move(pos, m);
nnue_acc_stack_pop(stack);
```

and call `nnue_acc_stack_reset(stack)` at the root of each iteration. A missing push or
pop does not crash — it silently evaluates the wrong position, which shows up only as a
moved node count.

## 5. Startup ordering

`nnue_feature_init()` must run once, in the same phase as `bitboards_init()` and
`attacks_init()`. Its tables are zero, not garbage, before it, so the failure mode is a
silent all-zero feature set.

`nnue_clear_refresh_cache(cache, nnue_ft_biases(ft))` must run after each net load and
before the first evaluate.

## 6. Arena ownership

`nnue_accumulator_stack_bytes()` and `nnue_refresh_cache_bytes()` size the two arenas;
both need 64-byte alignment. They are per-search-worker state, so when M4 lands the thread
pool each worker owns one of each. The feature-transformer blob is shared and read-only.
