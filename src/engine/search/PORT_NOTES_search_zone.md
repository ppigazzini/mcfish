# Port notes — the search-zone decomposition

The decomposed search zone IS the search. `search.c` is the facade over it — it
owns the public surface `search.h` declares and nothing else — and `search.h` is
unchanged, so `uci.c` and the test binary call what they always called.

Golden: `Stockfish/src/search.cpp`, `search.h`.

## Files and what each owns

| file | owns |
|---|---|
| `search_types.h` | `PVMoves`, `RootMove`, `Stack`, `NodeType`, `TbConfig`, `SearchZoneLimits`, `SearchTimeState`, `SearchCtx`, `SearchIdState` |
| `search_common.{h,c}` | every tuned margin / reduction / bonus formula, the value model, the reductions table, the TT adapter, the board predicates the search still carries, the seam storage |
| `search_setup.{h,c}` | stack sentinels, `SearchCtx` init, time-budget init, skill level, `SearchIdState` snapshot |
| `search_id.{h,c}` | the depth loop, the aspiration window, MultiPV, skill, the per-iteration time decision |
| `search_main.{h,c}` | Steps 1-12 of the alpha-beta node |
| `search_back.{h,c}` | Steps 13-21: the move loop and node finalization |
| `search_qsearch.{h,c}` | quiescence, plus the primitives it shares with the main search |
| `search_control.{h,c}` | `check_time`, `root_update`, `root_in_list`, `search_stopped`, `in_last_iter_pv` |
| `search_emit.{h,c}` | the info / bestmove / currmove lines and the MultiPV walk |
| `root_move_build.{h,c}` | the root move list and the Syzygy root ranking |
| `uci_wdl.{h,c}` | the win-rate model and the line formatters |
| `output_sink.h`, `option_source.h`, `time_source.h`, `tb_source.h` | the injection seams |

`search_main` and `search_back` form one component with one deliberate import
cycle — that cycle IS the recursion. Both headers say so; do not break it.

## What the facade owns

`search.c` maps `SearchLimits` onto `SearchZoneLimits`, registers the four seams,
builds the root move list, runs `iterative_deepening`, emits the final PV and
`bestmove`, and returns a `SearchResult`. It also keeps `perft`, which lives here
rather than in `movegen.c` because it shares the output sink.

It answers the option seam itself, with upstream's defaults (MultiPV 1, Skill Level
20, Move Overhead 10, everything else 0). The zone's own fallback answers 0 to all
of them, which reads as MultiPV 0 — no PV line searched at all — and Skill Level 0,
maximum handicap. Both are wrong searches rather than absent ones, so the defaults
have to come from somewhere until the decomposed shell registers the real model.

It resets `best_previous_score`, `best_previous_average_score` and
`previous_time_reduction` per `go` rather than carrying them between moves, using
upstream's own `ThreadPool::clear` values. Upstream resets them on `ucinewgame`;
the live UCI layer has no hook for that, and the same is true of the history block.
That per-`go` reset is also what makes two searches of one position node-identical,
which `test_search` asserts.

## Pruning steps: ported vs. deferred

Fully ported, with upstream's constants transcribed:

- Step 1-3 node init, mate-distance pruning, upcoming-repetition draw
- Step 4 TT probe, the TT cutoff with its `cut_node == (ttValue >= beta)` gate,
  the deep-TT verification search, and the 319d61eff depth penalty
- Step 5 static eval, correction history, `improving` / `opponent_worsening`,
  and the hindsight reduction adjustments
- Step 6 tablebase probe (behind `tb_config.cardinality`, so inert by default)
- Step 7 razoring, Step 8 futility, Step 9 null move + verification,
  Step 10 internal iterative reductions, Step 11 ProbCut, Step 12 deep ProbCut
- Step 13 move loop with `follow_pv`, Step 14 move-count / history / futility /
  SEE pruning for both quiets and captures, Step 15 singular extensions with the
  double and triple margins, Steps 16-18 the 1024-scaled LMR schedule and its
  re-searches, Steps 20-21 best-move bookkeeping, the stat updates, the TT store
  and the correction-history nudge
- The aspiration window, MultiPV, the aborted-MultiPV score repair, the
  forgotten-mate rollback, the skill handicap, and the per-iteration time model

Deferred, and each one is a node-count difference until it lands:

1. **`tt_pv` storage — RESOLVED.** `tt.c`/`tt.h` were re-ported: the generation
   narrowed to 5 bits, freeing bit 7 of `gen_bound8` for upstream's is-PV flag,
   and the replacement/aging policy is now upstream's. The
   flag round-trips through save and probe, and the node bodies that set it are
   live.
2. **TT miss depth — not a divergence.** At the pinned SHA upstream's
   `DEPTH_ENTRY_OFFSET` is `-3`. `tt.h` defines it and biases every stored depth
   by it, so `depth8 == 0` is the occupancy test.
3. **Optimism — RESOLVED.** `search_evaluate` passes `ctx->optimism[stm]` to
   `evaluate_with_optimism`, which scales the NNUE psqt / positional halves by it
   alongside material and `rule50` (upstream `search.cpp:1867`).
4. **Threads.** `check_time` gates on `ctx->nodes`, which IS the pool count at one
   thread. The pool sum, `increase_depth` across workers, thread voting and
   `best_move_changes` aggregation are single-worker shapes here.

## Dependencies on other zones

- **`pos_pseudo_legal` and `pos_gives_check` are still carried in the search
  zone**, as `search_pseudo_legal` / `search_gives_check` in `search_common.c`.
  Both are board-zone predicates, and `src/engine/board/legality.c` already holds
  the canonical `pos_pseudo_legal` — delete the copy in the commit that puts that
  module in `SOURCES`. `search_gives_check` additionally recomputes upstream's
  `StateInfo::checkSquares` on every call, because mcfish's `StateInfo` does not
  cache it; port `set_check_info`'s `check_squares[]` and this becomes a lookup.
- **`pos_non_pawn_material` is a function, not cached state.** Upstream reads
  `st->non_pawn_material[c]`. Step 14 calls it twice per move; caching it on
  `StateInfo` is a pure win with no behaviour change.
- **`pos_do_move`'s `gives_check` argument is accepted and not read** (position.h
  says so). `search_do_move` passes the computed value, so the search side is
  already correct for when it starts being trusted.
- **`src/engine/state/` is owned elsewhere.** This zone defines `RootMove` and
  `PVMoves` in `search_types.h` because `root_move_build` and the ID loop cannot
  exist without them. If `state/` lands its own definitions, `search_types.h`
  should include that header instead of declaring them — the field set here
  mirrors upstream's `RootMove` field for field, so the swap is mechanical.
  Likewise `SearchCtx` is this port's stand-in for upstream's `Worker`, and
  `SearchZoneLimits` for `LimitsType`; neither name collides with `state/`.
- **`SearchIdState.best_previous_score` / `best_previous_average_score` are not
  filled by `search_id_state_init`.** They are per-game manager state carried
  between `go` commands, which nothing owns yet; the caller must set them after
  init or the time model reads 0. Same for `previous_time_reduction`, which resets
  to 1.0 every search instead of persisting.
- **The seams' storage lives in `search_common.c`**, not in a `.c` per seam, so
  the zone links as one unit. The shell registers the real implementations by
  assigning the `Output*` / `Option*` / `Time*` / `Tb*` pointers.
- **`build.sh` carries every `.c` in this zone**, in both `SOURCES` and
  `ENGINE_SOURCES`, plus the two board-zone modules the emit path needs:
  `board_props.c` and `score.c`.

## The accumulator bracket

`search_do_move` / `search_undo_move` are the ONLY make/unmake in the zone that
touch the NNUE accumulator, and they are the only ones that may. Two sites do a raw
`pos_do_move` / `pos_do_null_move` on purpose:

- **the null move** (Step 9) — a null move moves no piece, so the child evaluates
  against this node's own slot. Pushing an empty diff is not equivalent, because a
  pushed diff is applied rather than skipped.
- **the Step 4 TT verification** — it makes the TT move, probes the next key, and
  unmakes. Nothing is evaluated in between, and upstream pushes no accumulator
  there either.

`search_do_move` also reads the moved piece **before** the make. Upstream indexes
the continuation pages by `DirtyPiece::pc`, which `Stockfish/src/position.cpp:848`
fills from `piece_on(from)` ahead of the move — the PAWN for a promotion, not the
piece standing on `to` afterwards.

## Verification

`./build.sh parity`. The per-position differential against a pristine upstream
build at fixed depth is the gate that actually holds this zone; a total that
matches while a position moves is a coincidence, not a pass.
