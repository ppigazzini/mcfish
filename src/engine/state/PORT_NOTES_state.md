# Port notes — engine/state

Changes needed in files outside this zone. Nothing here has been applied; every file
under `src/engine/state/` is new and edits no existing file.

## 1. `build.sh` does not know about this zone

The six new translation units must be added to `SOURCES` **and** to `ENGINE_SOURCES`,
or `zone-check` and the test binary will not see them:

```
src/engine/state/correction_bundle.c
src/engine/state/position_storage.c
src/engine/state/root_move.c
src/engine/state/shared_state.c
src/engine/state/worker_construct.c
src/engine/state/worker_histories.c
src/engine/state/worker_layout.c
```

**Status: all seven are now in `SOURCES` and `ENGINE_SOURCES`, so the zone links and
compiles under the full warning set.** It is still not *driven*: nothing constructs a
`Worker`, and no test does either. See
[docs/04-multithreading.md](../../../docs/04-multithreading.md).

## 2. `pos_do_move` is changing under us

`position.h`/`position.c` are owned elsewhere while `pos_do_move` grows
`DirtyPiece *` and `DirtyThreats *` parameters. This zone is written against the
**current** signature and does not call `pos_do_move` at all — `position_storage`
only owns storage, and `worker_construct` only writes fields.

The call site that will need the new signature is the search driver, which must
pass the two records `nnue_acc_stack_push` hands back for the ply it just pushed
(`nnue_accumulator.h:83`). No change is needed in this zone when that lands.

## 3. `SearchLimits` and `LimitsType` are two records for one thing

`src/engine/search/search.h` carries `SearchLimits` (depth, movetime, time, inc,
moves_to_go, nodes, infinite, ponder). `limits_type.h` carries upstream's full
`LimitsType`, which adds `searchmoves`, `mate`, `perft`, `npmsec` and `start_time`.

`search.h` was not edited, so both exist. When the search adopts the Worker, the
move is: `search_go` takes a `const LimitsType *`, `SearchLimits` is deleted, and
the UCI `go` parser fills `LimitsType` directly. `limits_type.h` deliberately does
not include `search.h` — it stays a POD leaf — so no conversion helper is offered
here.

## 4. `Histories` mixes per-worker and shared tables

`src/engine/search/history.h` defines one flat `Histories` block holding both the
per-worker tables (main, low-ply, capture, continuation, continuation-correction,
tt-move) and the two shared, key-indexed ones (correction, pawn). Upstream splits
them: the shared pair is one bank per NUMA node, sized to the node's thread count.

`worker_histories.h` embeds the whole block and reaches the shared pair only through
`SharedHistories`, so a worker that owns its node's bank binds to its own block
(`worker_histories_bind_own`) and one that does not binds to someone else's
(`worker_histories_bind_shared`). That is correct today and wastes ~17 MiB per extra
worker on a shared node.

The eventual change in `history.h`: split `Histories` into `WorkerTables` and
`SharedTables`, and have `history_clear` clear only the former. Two consequences
here:

- `worker_histories_clear` currently calls `history_clear`, which also refills this
  block's shared sub-tables. After the split it clears only the per-worker ones and
  the striped `worker_histories_clear_shared` becomes the sole shared-table writer.
- `worker_histories.c` repeats history.c's two shared fill values
  (`-6` correction, `-1262` pawn) because the striped clear writes a *range*, which
  `history_clear` does not express. They must move together on an upstream change.

Also: `history_clear` at `history.c:69` does not touch `low_ply_history`; it is
refilled per search by `history_fill_low_ply`. `worker_clear` matches that.

## 5. `tt.c` still owns a static table

`tt_types.h` splits out `TTCluster` and the `TranspositionTable` handle, and asserts
the 32-byte cluster. It does **not** duplicate the probe path — `tt.c` keeps a file-scope
table and `tt_probe`/`tt_store` take no handle.

`SharedState` and `Worker` hold a `TranspositionTable *` because that is upstream's
shape. Nothing dereferences it in this zone. When `tt.c` adopts the handle, the
requested surface is:

```c
TranspositionTable *tt_instance(void);
TTEntry *tt_probe_in(TranspositionTable *tt, Key key, bool *found);
void tt_new_search_in(TranspositionTable *tt);
```

## 6. `TablebaseConfig` belongs in the tablebase zone

`worker_layout.h` defines `TablebaseConfig` (cardinality, root_in_tb, use_rule50,
probe_depth) because `src/platform/tablebase.h` has no equivalent of upstream's
`Tablebases::Config`. It should move to `tablebase.h` when the root probing lands,
and `worker_layout.h` should include it instead.

## 7. `nnue_ft_ptr` returns untyped bytes

`worker_construct.c` casts `nnue_ft_ptr()`'s `const uint8_t *` to
`const NnueFeatureTransformer *` to reach `nnue_ft_biases`. A typed accessor in
`nnue_weight_storage.h` would remove the cast:

```c
const NnueFeatureTransformer *nnue_ft_current(void);  // null when no net is resident
```

## 8. The reduction table has no owner yet

`search.c` has no reductions table — the late-move reduction is computed inline.
`worker_fill_reductions` ports the upstream fill (`search.cpp:696`,
`int(2834 / 128.0 * std::log(i))`) into the Worker, where
upstream keeps it. When the search adopts the Worker it must read `w->reductions`
rather than recomputing, or the two schedules can drift.

## Untranslatable / deliberately not ported

- **A pinned Worker offset table** (`worker_off`, and pinned
  `worker_size`/`position_size`/`state_info_size` constants). They would only be
  needed to reinterpret an externally allocated Worker image by byte offset. In C
  the struct *is* the layout, so the offsets are the compiler's and the pinned
  sizes would be meaningless. The one runtime layout that remains — where the two NNUE arenas sit
  inside the block — is computed from `nnue_accumulator_stack_bytes()` /
  `nnue_refresh_cache_bytes()` and exposed as `worker_accumulator_stack_offset()` /
  `worker_refresh_table_offset()`, never as a constant.
- **The page allocator** is already ported as `page_alloc` / `page_free` /
  `page_alloc_set` in `src/platform/memory.h`. This zone calls it and adds nothing.
- **`RootMove::extract_ponder_from_tt`** (search.h:128) needs a TT probe and a
  Position make/unmake. It belongs in the search, not in the root-move type.
- **`ISearchManager` / `NullSearchManager`** collapse to a nullable `SearchManager *`:
  the only virtual is `check_time`, and the null object exists solely to avoid a
  branch that C expresses as `w->manager != nullptr`.
