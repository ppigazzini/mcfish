# Engine: the board

Everything under [`src/engine/board/`](../src/engine/board): the value types and the
move encoding, the bitboard vocabulary and the magic slider tables, the
`Position`/`StateInfo` split, Zobrist hashing, the per-move threat deltas, FEN,
move generation, and legality.

Audience: engine contributors. The search's use of these APIs is in
[02-engine-search.md](02-engine-search.md).

## Where this zone stands

The board zone is the closest to done of the three: perft is green on the
reference set, the slider lookup is upstream's magic-bitboard one, the Zobrist
tables are drawn upstream's way, and the per-move threat deltas the NNUE feature
set consumes are recorded on every make and unmake.

**Wired into the binary** ([`../build.sh`](../build.sh) `SOURCES`):
[`bitboard.c`](../src/engine/board/bitboard.c),
[`attacks.c`](../src/engine/board/attacks.c),
[`repetition.c`](../src/engine/board/repetition.c),
[`threats.c`](../src/engine/board/threats.c),
[`position.c`](../src/engine/board/position.c),
[`movegen.c`](../src/engine/board/movegen.c),
[`uci_move.c`](../src/engine/board/uci_move.c).

**M1's module split is done.** `zobrist.c`, `state_list.c`, `legality.c` and
`fen.c` are all in `SOURCES` and own their behaviour; `position.c` no longer
carries a second copy of any of it.

Three extracted modules were **deleted** rather than wired, each superseded by
live code: `position_query.c` and `position_snapshot.c` duplicated queries
[`board_props.c`](../src/engine/board/board_props.c) already owns, and
`fen_parse.c` was a second FEN parser with no `pos_set_reason` — the entry the
shell needs and whose reason string `tools/errors.golden` pins. `position.c`
keeps the decode side until its shared helpers (`set_check_info`,
`slider_blockers`, `compute_key`, used by both `pos_set` and `pos_do_move`) get a
header of their own.

**Those extractions are wired, not merely on disk.** `position.c` no longer carries
a second copy of `pos_fen`, `pos_pretty` or `pos_legal` — those live in `fen.c` and
`legality.c` now — and it draws its Zobrist tables by calling `zobrist_init` rather
than generating its own. `pos_set` stays in `position.c`, which keeps the decode
helpers (`set_check_info`, `slider_blockers`, `compute_key`) it shares with
`pos_do_move`. The split landed as one commit that deleted from `position.c` as it
added to `SOURCES`, so the tree was buildable at every step.

The Zobrist draw is the load-bearing subtlety the split had to preserve:
`zobrist_init` draws for the twelve real pieces only, skipping the two encoding
gaps, because drawing all fourteen would shift every key from `B_PAWN` onward off
upstream's table. See *Zobrist* below for why that distinction is the load-bearing
one.

Board fidelity is proven when perft matches on a randomised sweep against the
upstream binary (`./build.sh upstream-nodes`), not merely on the reference table
(`./build.sh perft`).

## Types and the move encoding

[`types.h`](../src/engine/board/types.h) defines the whole value domain. Every type
is a fixed-width integer with a named total bound (`SQUARE_NB`, `PIECE_NB`,
`MAX_MOVES`, `MAX_PLY`). The width is load-bearing on both sides: a `Piece` has to
pack into `board[64]` and a `Square` indexes every attack table, so the type and
its `*_NB` companion are one fact written twice. Widening one without the other
leaves an in-range value that indexes past the end of a table — no diagnostic,
and the sanitizer only sees it on a position that reaches the high squares.

Two encodings carry most of the weight:

**Pieces are `color << 3 | type`.** `NO_PIECE` is 0, White runs 1..6, Black 9..14,
and 7 and 15 stay unused. The gap is deliberate: it keeps the colour bit at a fixed
position, so `color_of_piece` is `pc >> 3` and `type_of_piece` is `pc & 7` — shifts,
not table lookups. It also makes `PIECE_NB` 16, so `piece_count[PIECE_NB]` is
indexed directly by a `Piece` with no compaction step.

**Squares are rank-major A1..H8.** `rank_of` is `s >> 3`, `file_of` is `s & 7`, and
`flip_rank` is `s ^ SQ_A8` — the vertical mirror the evaluation needs for Black.
`relative_rank(c, s)` is `rank_of(s) ^ (c * 7)`, which makes pawn logic
colour-agnostic without a branch.

### The 16-bit move

A `Move` is a `uint16_t` packed as:

```
type << 14 | (promo - KNIGHT) << 12 | from << 6 | to
```

- bits 0-5: destination square
- bits 6-11: origin square
- bits 12-13: promotion piece, biased by `KNIGHT` so the four promotions fit in two bits
- bits 14-15: `MoveType` — `NORMAL`, `PROMOTION`, `EN_PASSANT`, `CASTLING`

Accessors are `move_from`, `move_to`, `move_type`, `move_promotion`; constructors
are `make_move` (normal moves only) and `make_move_typed`.

**`MOVE_NONE` and `MOVE_NULL` are the `from == to` trick.** `MOVE_NONE` is 0
(a1→a1) and `MOVE_NULL` is 65 (b1→b1). No legal chess move has `from == to`, so
those two encodings can never collide with a real move, and the sentinels cost no
extra bit and no separate `bool`. `move_is_ok(m)` is exactly `move_from(m) !=
move_to(m)`.

The invariant that keeps this safe: **`make_move` and `make_move_typed` must never
be called with `from == to`.** Nothing enforces it — a generator that emitted a
null-step move would produce a value the search reads as "no move" and the TT stores
as "no best move", with no diagnostic anywhere.

The 16-bit value is also a **direct index** into the butterfly history tables in
[`../src/engine/search/history.c`](../src/engine/search/history.c), which is why
the encoding is not free to change: the tables are sized by it.

Two consumers must know the encoding is not the wire format:

- Castling is stored **king-captures-rook**: `to` is the rook's square, not the
  king's destination. `move_to_uci` in
  [`uci_move.c`](../src/engine/board/uci_move.c) converts to the king's real
  destination for standard chess and leaves the rook square for Chess960. That
  asymmetry lives in `uci_move.c` and nowhere else.
- `make_move_typed` for `EN_PASSANT` and `CASTLING` passes `KNIGHT` as the promotion
  argument, which encodes as the two zero bits. The field is meaningless for those
  types; do not read it.

## Bitboards

[`bitboard.h`](../src/engine/board/bitboard.h) /
[`bitboard.c`](../src/engine/board/bitboard.c). A `Bitboard` is a `uint64_t` in
which bit `s` means square `s` is occupied.

**This header is the std-only leaf, and it must stay one.** It holds the square
masks, the scan intrinsics and the set shifts, and it holds **no attack tables** —
those live in [`attacks.h`](../src/engine/board/attacks.h), which includes this
one and not the reverse. `bitboard.c` fills exactly one table, `SquareBB`. Adding
an attack table here would make the leaf depend on the magic search that is built
on top of it.

The scan helpers are `lsb`, `msb`, `pop_lsb`, and `popcount_bb`, all `static inline`
wrappers over clang's `__builtin_ctzll` / `__builtin_clzll` / `__builtin_popcountll`.
`lsb`, `msb`, and `pop_lsb` carry a precondition the signature cannot state:
**`b != 0`.** `__builtin_ctzll(0)` is undefined, so an empty-board scan is not
"returns 64", it is UB — the wrappers inherit the builtin's domain and add no
guard, because a branch on every scan is a cost the hot path will not pay. The
caller establishes the precondition, which is why `pop_lsb` loops are always
written `while (b) { ... pop_lsb(&b) }` and never `do/while`: the loop condition
*is* the check. Nothing but review enforces this — write the `while` form.

`shift_bb(d, b)` moves the whole set one step and drops the bits that wrap: the
east/west cases mask off the H- and A-files first. `pawn_attacks_bb` is two shifts
ORed. `bb_more_than_one(b)` is `(b & (b - 1)) != 0` — used by the evasion generator
to detect double check without a popcount.

## Slider attacks: two algorithms, split by ISA

[`attacks.h`](../src/engine/board/attacks.h) /
[`attacks.c`](../src/engine/board/attacks.c) own the slider lookup and the derived
square-pair geometry.

**Upstream switches slider ALGORITHM at avx2, and so does this zone.** The gate is
`attacks.h:35` upstream (`#elif defined(USE_AVX512ICL)` … `USE_DUAL_HYPERBOLA_QUINT`)
and `__AVX2__` here. Getting this wrong is not a micro-optimisation: the port ran the
magic path at every tier for its whole life, which is upstream's *pre-avx2* code, and
that single omission accounted for the per-node deficit tracking the ISA tier — near
parity at sse41, where both engines use magics, and 8–14% above it.

### At avx2 and above: dual hyperbola quintessence

`both_attacks_bb(s, occupied)` returns **both** ray sets in one pass. A 32-byte
`DualMagic` per square holds the file, diagonal and antidiagonal masks; one 256-bit
load, one subtract, one byte-reversed subtract and an xor produce all three rays
together, and the rank — the one direction the byte reversal cannot serve, because a
rank's eight squares share a byte — comes from a 256-entry lookup.

The whole slider dataset is **3 KiB and L1-resident**. The magic path's is **841 KiB**
(`RookTable[0x19000] + BishopTable[0x1480]`) walked at random. Upstream does not even
compile its magic tables in this configuration (`attacks.cpp:28`).

Five sites want both sets and take the dual call, matching upstream exactly:
`threats_update_piece` (`position.cpp:1203`), `set_check_info` (`:473`),
`attackers_to` (`:645`), `gives_check`'s en-passant case (`:796`) and `see_ge`'s queen
branch (`:1505`). `see_ge`'s pawn and bishop branches uncover diagonals alone and its
rook branch files alone, so they stay single-ray — there too.

### Below avx2: the magic tables

`attacks_bb(pt, s, occupied)` dispatches on the piece type. Leapers read the
precomputed `PseudoAttacks[pt][s]` and ignore `occupied`. Sliders take upstream's
magic path — mask, multiply, shift, one dependent load:

```c
static unsigned magic_index(const Magic *m, Bitboard occupied) { ... }
```

A `Magic` is `{ mask, attacks, magic, shift }`, where `attacks` points **into** a
shared flat table rather than owning storage: each square's block starts where the
previous square's ended, so `RookTable` and `BishopTable` are one allocation each
and a lookup is a single indexed read.

Three invariants hold this together, and each has a silent failure mode:

- **The flat table sizes are upstream's constants, not headroom.** `RookTable` is
  `0x19000` entries and `BishopTable` is `0x1480`, which are exactly the sums the
  mask popcounts imply. Under-size either and `init_magics` writes past the end of
  the previous square's block — a heap-shaped bug inside a file-scope array, so
  ASan sees nothing.
- **The tables are read-only during search.** `attacks_init` is the only writer and
  runs once, single-threaded, before any `Position` exists. That is what makes them
  safe to share across the pool's workers, which now run; a lazily-filled table
  would not be.
- **`Magics` is cache-line aligned, and a square's pair must stay one line.**
  `sizeof(Magic)` is 32, so `Magics[s][0..1]` — bishop then rook — is exactly 64
  bytes, and the `alignas(64)` is what puts that pair on a line boundary instead of
  straddling two. Every site that probes both sliders from one square depends on it
  (`pos_attackers_to_occ`, `set_check_info`, `see_ge`, `threats_update_piece`), and
  it matters most on the non-BMI2 path, which reads `magic` and `shift` as well as
  `mask` and `attacks`. A `static_assert` pins the pair-fits-a-line half, because
  widening `Magic` would otherwise break it in silence.
- **The magic search's generator and seeds are fixed.** `prng_rand64` is
  xorshift64* and `prng_sparse_rand` ANDs three draws so candidates are sparse.
  Different magics are equally correct and produce an incomparable table dump, which
  is why the generator is not to be "improved".

`sliding_attack` — the four-direction ray walk — stays in the binary. It is not
dead code and not a fallback: it is the reference `init_magics` validates each
candidate against while building the table. `safe_step` guards on **file
distance**, not just index range: `NORTH_EAST` from H4 computes an index that is on
the board (A5) and geometrically wrong, so the check is `|df| <= 2` rather than
`0 <= to < 64`.

`attacks_bb` is a pure function of `(pt, s, occupied)`, which is what makes the
slider path replaceable without moving the anchor: any correct implementation must
leave `./build.sh signature` green. If a change there moves the node count, the
new lookup is wrong, not faster.

To claim a speed change, measure it:

```bash
./build.sh bench 8      # prints Total time / Nodes searched / Nodes/second on stderr
```

Record `Nodes/second` before and after, on an idle machine, and quote both. The
node total must not move.

### The derived pair tables

`attacks_init` also builds `BetweenBB` and `LineBB` from the empty-board rays:

- `LineBB[s1][s2]` — the full line through both squares, or 0 when they are not
  aligned. `aligned(s1, s2, s3)` tests `LineBB[s1][s2] & square_bb(s3)`, which is
  the pin-ray test in `pos_legal`.
- `BetweenBB[s1][s2]` — the squares strictly between, **plus `s2` itself**, and it
  defaults to just `square_bb(s2)` when the pair is not aligned. Including the
  endpoint is what lets the evasion generator use one mask for both replies to a
  slider check: block it, or capture it.

Both tables are `Bitboard[64][64]`, filled once at startup.

## Position and StateInfo

[`position.h`](../src/engine/board/position.h) /
[`position.c`](../src/engine/board/position.c).

The split is the core design decision on this page:

- **`Position`** holds what a move rewrites in place: the `board[64]` mailbox,
  `by_type` (index 0 is the total occupancy, aliased as `ALL_PIECES`), `by_color`,
  `piece_count`, `side_to_move`, `game_ply`, the castling lookup arrays including
  `castling_path`, the `chess960` flag, a pointer to the current `StateInfo`, and the
  two scratch NNUE delta slots described under *Threat deltas* below.
- **`StateInfo`** holds what a move cannot cheaply recompute on the way back: the
  Zobrist `key`, the four auxiliary keys, `repetition`, `rule50`,
  `plies_from_null`, `ep_square`, `castling_rights`, `captured_piece`, and the
  derived `checkers` / `blockers` / `pinners` sets, plus a `previous` pointer.

### The field ORDER of both is upstream's, and it is load-bearing

Two members are placed by cache line, not by taste, and both were found by putting
`offsetof` side by side against the oracle rather than by any behavioural test:

- **`board` is declared FIRST.** It is `Piece[64]` — exactly one cache line — so only
  at offset 0 does `piece_on` touch a single line per read. Declared after the
  bitboards it began at offset 72 and straddled the 64/128 boundary.
- **`StateInfo`'s tail runs `key, checkers, previous, blockers, pinners,
  check_squares, captured_piece, repetition`**, giving `key` at 64 and `previous` at
  80. The repetition walk steps `st = st->previous` and reads `st->key` at every step,
  and it runs on the order of a million times a search (`pos_is_draw` alone is called
  ~2.3M times on the depth-12 bench). With `previous` at the end of the struct — where
  it used to be — each step of that pointer chase touched two lines where upstream's
  touches one.

`offsetof(StateInfo, key)` is 64 on both engines, so `pos_do_move`'s forward copy
moves the same bytes.

**`castling_path` is precomputed, not derived.** Upstream stores the squares each
castling right needs empty when the right is set (`position.cpp:462`) and tests it
with one AND (`position.h:283`). The port used to rebuild that path inside
`generate_castling` on every call — two random lookups into the 32 KiB `BetweenBB`
table plus five bitboard ops, per side, per node. Identical call counts, identical
results, invisible to every gate; the `sizeof` comparison against upstream (744 bytes
against 1056) is what exposed it.

**Undo restores by popping, never by recomputing.** `pos_undo_move` moves the pieces
back, then does `pos->st = pos->st->previous`. It never recalculates the key, the
rights, or the ep square. The invariant that follows is the one to write down before
touching either struct:

> Every field added to `StateInfo` must be written by `pos_do_move` before the
> recursion. A field left unwritten is restored *stale*, because `memcpy` copied the
> parent's value forward and nothing overwrote it.

That failure is invisible to perft — perft only counts leaves, and a stale field
that does not change move generation counts correctly. It is caught by
`walk_roundtrip` in [`../tests/test_main.c`](../tests/test_main.c), which snapshots
the boards, the key, the rights, and the ep square before each move and `memcmp`s
them after the undo.

The chain's lifetime is the caller's problem. `pos_do_move` takes a `StateInfo *`
the caller owns; the search passes an automatic in its own frame (valid for exactly
the recursion below it), and the shell passes a slot from a `States[MAX_GAME_PLIES]`
file-scope array so the game history survives across commands. A `StateInfo` popped
off the C stack while still linked would leave `previous` dangling, and the
repetition scan follows `previous`.

### The four auxiliary keys

`StateInfo` carries `material_key`, `pawn_key`, `minor_piece_key` and
`non_pawn_key[COLOR_NB]` beside the main `key`, each maintained incrementally by
`toggle_aux_keys` and defined from scratch by `compute_key`. **The two definitions
are one fact written twice and must not drift.**

Their membership rules are upstream's and are not what the names suggest:

| Key | Contains |
| --- | --- |
| `pawn_key` | pawns only, seeded with `Zobrist_no_pawns` so an empty pawn structure still has a distinct key |
| `non_pawn_key[c]` | every non-pawn of colour `c`, kings **included** |
| `minor_piece_key` | knights and bishops only, kings **excluded** |
| `material_key` | piece counts with no square information: `Zobrist_psq[pc][8 + cnt]` for each copy |

Putting a king into or out of the wrong one silently mis-indexes a correction or
pawn history table — it costs strength and fails no gate. `material_key`'s offset
of 8 keeps the count slots clear of the square range of the same Zobrist row, and
Syzygy looks its tables up by that key, so it is not decoration.

`StateInfo::repetition` is the other field whose encoding is the whole
mechanism: it is the ply distance back to the previous occurrence of this
position, **negative** when that occurrence was itself a repetition (so this is the
threefold), and 0 when the position is new. Every query in
[`repetition.c`](../src/engine/board/repetition.c) reads that sign and none
recomputes it.

### Zobrist and the fixed seed

`position_init` fills `Zobrist_psq[PIECE_NB][SQUARE_NB]`,
`Zobrist_enpassant[FILE_NB]`, `Zobrist_castling[16]`, `Zobrist_side` and
`Zobrist_no_pawns` from a xorshift64* generator seeded with a **hardcoded
constant**.

Two properties of the draw order are upstream's and are load-bearing:

- **Twelve pieces are drawn, not fourteen.** The loop walks a 12-element `Pieces[]`
  and skips the encoding gaps at 7 and 8. Drawing for the gaps would consume 128
  extra PRNG values and shift every key from `B_PAWN` onward off upstream's table —
  correct chess, wrong keys, and no gate here would notice until the differential
  gate at M6.
- **The pawn promotion rows are zeroed.** `Zobrist_psq[W_PAWN][rank 8]` and
  `Zobrist_psq[B_PAWN][rank 1]` are set to 0 after the draw. A pawn never rests
  there, so the entry is unreachable from `compute_key`; zeroing it is what lets the
  promotion XOR in `pos_do_move` cancel implicitly rather than needing a special
  case.

The seed must not vary — not per run, not per platform, not per build. The bench
signature, every UCI golden, and the TT hit pattern are all functions of these keys.
Seeding from `time()`, `rand()`, or anything the host supplies would make
`./build.sh signature` fail nondeterministically and every golden meaningless, and
the failure would look like a search bug. If you need different keys, change the
constant and regenerate the anchors deliberately — see
[09-tooling-ci.md](09-tooling-ci.md).

The key is maintained incrementally in `pos_do_move` and recomputed from scratch by
`compute_key` only in `pos_set`. The two must agree; `walk_roundtrip` asserts it by
round-tripping the position through FEN after every move and comparing the
recomputed key with the incremental one.

## Threat deltas

[`threats.h`](../src/engine/board/threats.h) /
[`threats.c`](../src/engine/board/threats.c) record, for every make and unmake, the
(attacker, attacked) pairs that appeared or disappeared. That list is what the NNUE
`full_threats` feature set consumes; see [03-engine-eval.md](03-engine-eval.md).

`threats_init` builds `RayPassBB`, the geometry the discovered-threat scan walks,
and it reads the attack tables — hence its place in the init order in
[00-architecture.md](00-architecture.md).

**At the ICL tier the list is written vectorised**, as upstream's
`write_multiple_dirties` does (`position.cpp:1157`, gated on `USE_AVX512ICL`; here on
`__AVX512VBMI__ && __AVX512VBMI2__`). The scalar form is up to sixteen iterations of
`pop_lsb` → `board[sq]` → pack → append, each dependent on the last, in the hottest
board function there is — `threats_update_piece_ray` runs ~3.5M times on the
depth-12 bench. The vector form collapses it: the whole 64-square board is one
register (it is exactly 64 bytes), `compress_epi8` turns the bitboard into the packed
square list in one instruction, `permutexvar_epi8` looks up all sixteen pieces from
the board register with no memory access, and one `ternarylogic` ORs the constant
template together with both shifted fields.

**It is not a drop-in, and the control flow moves with it.** On the vector path the
direct sliders are folded into the *incoming* mask and written by the same call, and
`process_sliders` is then told not to emit them again (upstream `position.cpp:1273`
and `:1293`); the scalar path folds them in only on the `!compute_ray` branch. Both
shapes live side by side under the same gate.

The 64-byte store is unmasked, which is why `DIRTY_THREAT_MAX` is 96 rather than the
80 the feature bound needs — the 16-slot tail exists so that store cannot leave the
array.

Three things a caller must know:

- **Call ordering is the whole correctness argument.** The threat call must run
  with the position holding the *other* side of the transition: before the boards
  change for a removal, after they change for a placement. That ordering is what
  makes `occupied` correct in both directions, and it is why the calls sit where
  they do inside `put_piece` / `remove_piece` / `move_piece` in `position.c` rather
  than being hoisted to the top or bottom of `pos_do_move`.
- **The recorded set must be exactly what the feature indexer encodes, never a
  superset.** The filters in `threats.c` are not an optimisation: the pairs they
  drop are the ones whose index maps out of range and which the accumulator then
  discards. Recording a pair upstream rejects moves the eval.
- **The list is bounded at 96 and never checked.** A non-castling move changes at
  most 80 features and a castling move at most 36, so 80 bounds it; the remaining
  slots exist so an unmasked vector store near the end stays in bounds.

`pos_do_move` writes the deltas through caller-supplied `DirtyPiece *` and
`DirtyThreats *` out-parameters — the slots an accumulator hands out, so nothing is
copied. A caller with no accumulator passes `&pos->scratch_dp` and
`&pos->scratch_dts`, which is what the live search does. Those scratch fields live
only between `pos_do_move` and the read of them; `pos_undo_move` does not restore
them.

`pos_do_move` also takes a `Histories *history` parameter (`nullptr` where none is
available). When non-null, the make uses it to prefetch the child position's TT
cluster and correction-history bundles from inside the make itself, the same point
upstream issues its prefetch, rather than as a separate step the caller has to
remember. `pos_adjust_key50_of` (`position.h`) is the piece that makes the
prefetch address computable before the move is fully applied: it folds the
halfmove clock into the key past move 14, the same rule TT lookups apply, so the
prefetched cluster is the one the search will actually probe.

`pos_do_null_move` fills both with the empty delta rather than leaving them alone,
so the incremental accumulator step is a no-op instead of a read of a stale ply's
diff.

### FEN

`pos_set` parses a full FEN record and returns `false` on anything malformed,
leaving `pos` unspecified. It rejects: a rank that does not sum to 8, too few or too
many ranks, a bad side-to-move token, and — the one that is not syntax — **any
position without exactly one king per side**, because every downstream
`king_square()` is an `lsb` on the king bitboard and would read square 0 from an
empty board. The rejection list in `test_fen` in
[`../tests/test_main.c`](../tests/test_main.c) has one FEN per invariant.
`pos_set_reason` is the same parse with the diagnostic upstream prints, and the
messages are upstream's verbatim — a reason that only approximates the reference is
not comparable against it.

**A FEN IS A CHARACTER STREAM, NOT A LIST OF WHITESPACE-SEPARATED FIELDS.** Upstream
reads it under `noskipws`, one character at a time, so a field ends at exactly the
one character that ends it and the character after that already belongs to the next
field. Three consequences that a split-on-whitespace reading gets wrong, all of them
reachable from a GUI:

- `-` ends the castling field, and only in first position. `w -K - 0 1` therefore
  reads `K` as an EN-PASSANT square and fails; a `-` *after* a right is not a
  separator but an unrecognised right, so `w K- - 0 1` fails too.
- the side-to-move field must be followed by exactly one whitespace character.
- the stream running dry inside the board field is `Unexpected end of stream`, and
  that is reported before the board-shape check, so `8/8/8` is an end of stream
  rather than a half-drawn board.

Castling rights accept both standard (`KQkq`) and Shredder-FEN file letters
(`A`..`H`), and **incorrect rights are sanitised rather than rejected** — upstream
says so in the source, because stale castling fields are common enough that it
relaxed the rule for them. A right whose rook or king is missing is DROPPED and the
rest of the position is accepted; only an unrecognised character is an error. `K`/`Q`
mean "scan inward from that corner and take the first rook, stopping AT the king",
not "the h-file/a-file rook": the king must come later than the rook or the right is
not real, so with `Kh1` and `Ra1` the kingside right is dropped rather than granted
to a rook on the far side of the king. A file letter looks for its rook on the BACK
RANK and for the king on files b..g only, since a king on the a- or h-file has no
room to castle. `set_castling_right` records the rook origin in
`castling_rook_square[cr]` and marks both the king and rook squares in
`castling_rights_mask`, which is what lets `pos_do_move` drop a right by masking on
the from- and to-square together.

**The en-passant field is stored only when the capture is actually available, and a
square that fails the SYNTAX is a different outcome from one that fails the test.**
The rank is checked against the side to move — relative rank 6, so `6` for white and
`3` for black — and a square on the wrong one is a rejected FEN, not a dropped
square. Past that, four conditions decide whether `ep_square` is recorded at all: a
pawn of the side to move attacks the square, the enemy pawn stands in front of it,
neither the square nor the one behind it is occupied, and at least one capturer can
execute the capture **without leaving its own king in check** — a pawn pinned along
the fifth rank cannot take, and if none of them can then the square is not an
en-passant square. Otherwise `ep_square` stays `SQ_NONE`, and `pos_do_move` applies
the same rule when setting the square after a double push. This matters because the
ep square is hashed into the key: a FEN that states an ep square unconditionally
would produce a different key for a position that is, in every way that affects play,
identical — desynchronising the TT, the repetition test, and every golden built on
them. The two sites must stay in agreement; changing one alone breaks the FEN
round-trip in `test_fen`.

`pos_fen` writes the record back, and `pos_pretty` renders the ASCII board plus the
FEN and key that the UCI `d` command prints.

## Move generation

[`movegen.h`](../src/engine/board/movegen.h) /
[`movegen.c`](../src/engine/board/movegen.c).

`generate(pos, list, type)` appends moves at `list` and returns the new end pointer.
`list` must have room for `MAX_MOVES` entries — the generators do not bounds-check,
so only the caller's declaration keeps the write in bounds. Declare the buffer
`ExtMove list[MAX_MOVES]` and nothing smaller; a short buffer overruns silently in
the release build and is caught only if the `test` step's ASan run happens to
reach the position.

The four `GenType`s:

| Type | Emits |
| --- | --- |
| `GEN_CAPTURES` | captures and **queen** promotions |
| `GEN_QUIETS` | everything else, including under-promotions |
| `GEN_NON_EVASIONS` | both of the above; used when not in check |
| `GEN_EVASIONS` | replies to a check, masked to the block-or-capture target |

Captures and quiets partition the non-evasion set exactly — `test_legality` asserts
`cn + qn == pn` on three positions. The queen promotion sits in the capture set
because it is the only promotion the quiescence search wants to see.

**Everything except `generate_legal` is pseudo-legal.** The generators may leave
your own king in check, and every caller must filter with `pos_legal`. Only
`generate_legal` — which generates into a local buffer and copies through the filter
— is safe to iterate raw. It is the perft and root generator; the search filters
inline so it can skip the copy.

Evasion generation short-circuits on double check: if `bb_more_than_one(checkers)`,
it skips the pawn and piece generators entirely and emits only king steps, rather
than generating moves that `pos_legal` would all reject.

The en-passant case inside the evasion path is the subtle one: the captured pawn
sits *behind* the ep square, so the target test is on that pawn's square, not on the
landing square. Getting it backwards produces a legal-move count that is right
almost everywhere and wrong in exactly the positions perft position 3 covers.

Castling is emitted by `generate_castling`, which checks only that the path is
clear — with both movers masked out of the path, because in Chess960 the king or
rook may already stand on a destination square. Whether the king walks *through*
attack is `pos_legal`'s job, not the generator's.

## Legality

`pos_legal(pos, m)` takes a move that is already pseudo-legal and answers whether it
leaves our king safe. It has four cases, and three of them are special for a reason:

**En passant** — two pieces leave the board in one move (the mover and the captured
pawn, on different squares), so no precomputed pin set covers it. The test rebuilds
the occupancy by hand and asks directly:

```c
const Bitboard occ = (pieces(pos) ^ square_bb(from) ^ square_bb(cap)) | square_bb(to);
```

then checks for a rook/queen or bishop/queen attack on the king through that
occupancy. This is the classic "capturing en passant exposes the king along the
rank" position, and it is why the double removal must be explicit.

**Castling** — walks each square the king crosses and rejects if any is attacked.
`to` holds the rook square, so the king's real destination is derived first
(`make_square(king_side ? 6 : 2, rank_of(from))`).

**King moves** — test the destination against an occupancy with the king's origin
square cleared. Without that clear, a slider checking the king along a line looks
blocked by the king itself, and the engine happily steps backwards along the check.

**Everything else** — one line:

```c
return !(pos->st->blockers[us] & square_bb(from)) || aligned(from, to, ksq);
```

A non-king mover is legal iff it is not a blocker, or it stays on the pin ray.
`blockers[us]` is computed once per position by `slider_blockers` (called from
`set_check_info` after every make/unmake), which finds each enemy slider that would
see the king on an empty board and marks the single piece in the way — exactly one
piece; two or more block each other and neither is pinned.

Note that `blockers[c]` may contain pieces of *either* colour: an enemy blocker is
the piece that could discover a check by moving away. `pinners[c]` holds the sliders
and is filled only for own-colour blockers.

`pos_pseudo_legal` is the guard in front of `pos_legal`, not a convenience.
`pos_legal` is **undefined** on a move that could not have been generated, so any
move from an untrusted source — a TT hit, a killer, a UCI string — must pass
`pos_pseudo_legal` first. The live search does exactly that with its TT move before
handing it to the picker.

`pos_see_ge(pos, m, threshold)` answers whether a move's static exchange evaluation
clears a threshold without generating the exchange sequence. The move picker prunes
captures with it; see [02-engine-search.md](02-engine-search.md).

## Draw and repetition detection

[`repetition.h`](../src/engine/board/repetition.h) /
[`repetition.c`](../src/engine/board/repetition.c) own every draw query, and none
of them recomputes anything: they read `StateInfo::repetition` — which
`pos_do_move` writes — and walk the `previous` chain.

The chain must therefore be live for `min(rule50, plies_from_null)` plies back,
which is exactly what a root `StateInfo` with `plies_from_null == 0` guarantees.
Every walk dereferences `previous` unguarded on that basis, as upstream does. A
position set mid-game from FEN has a truncated chain, and it is the
`plies_from_null` bound — not a null check — that keeps the walk inside it.

`pos_is_draw(pos, ply)` is the query the live search calls: the fifty-move rule
plus repetition, where `ply` bounds the "inside the current search line" window in
which a *single* repetition already scores as a draw. Outside that window the full
threefold is required. Passing a wrong `ply` does not crash — it changes which
positions the search calls drawn, which moves the node signature.

`pos_is_repetition` and `pos_has_repeated` are the root-side queries, and
`pos_upcoming_repetition` is the cycle-detection test that probes the cuckoo table.

**The cuckoo table is built inside `position_init`**, at the end, from the psq and
side keys it has just drawn. It takes them as arguments rather than re-deriving
them: two independently seeded copies of the same PRNG is exactly the drift this
table cannot survive. `pos_upcoming_repetition` is probed at the top of every
non-root node in
[`../src/engine/search/search_main.c`](../src/engine/search/search_main.c), so a
zeroed table is not an inert gap — it turns a cycle cutoff into a silent no-op that
costs nodes and fails no gate.

## UCI move strings

[`uci_move.c`](../src/engine/board/uci_move.c) is the only place that knows the
castling wire-format asymmetry. `move_to_uci` prints `(none)` for `MOVE_NONE` and
`0000` for `MOVE_NULL`, otherwise four characters plus a promotion letter.

`move_from_uci` parses by **generating all legal moves and comparing rendered
strings**. That is O(legal moves) per parse and is the reason an illegal or
malformed token and a well-formed-but-illegal move both return `MOVE_NONE` through
the same path — the parser has no notion of "syntactically valid". It also means a
move can only be parsed against the position it applies to, which is why
`cmd_position` in [`../src/shell/uci.c`](../src/shell/uci.c) applies moves one at a
time as it walks the `moves` list.
