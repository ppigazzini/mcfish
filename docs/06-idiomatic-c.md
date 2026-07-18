# Idiomatic C23 here

The C23 the tree commits to, the warning set that enforces it, why there is no
build system, and — because it is the daily work of this repo — the recurring
patterns for turning zfish's Zig into C23.

Audience: hot-path and build contributors. The gates that hold these rules are in
[07-tooling-ci.md](07-tooling-ci.md); the port sequence is in [PORTING.md](PORTING.md).

**Path convention on this page.** A zfish module is written relative to zfish's
own `src/`, as *zfish `engine/eval/network.zig`*; a Stockfish golden is written
relative to Stockfish's `src/`, as *upstream `nnue/network.cpp`*. Neither is a
path in this repository, and writing them bare would make
`./build.sh docs-lint` assert they exist here.

## C23, pinned and probed

[`../build.sh`](../build.sh) selects the C23 flag by **probing the compiler**,
not by pinning a version:

```bash
for f in -std=c23 -std=c2x; do ... done
```

GCC only learned `-std=c23` in 14; GCC 13 accepts the same language as
`-std=c2x`. Probing is what lets the second-compiler lane in
[`../.github/workflows/ccfish_parity.yml`](../.github/workflows/ccfish_parity.yml)
run on a stock toolchain. **There is no fallback to a pre-C23 mode**, and that is
deliberate: an older mode silently accepts `nullptr` and fixed-underlying-type
enums as extensions, with different diagnostics and, for the enums, potentially a
different underlying type. The probe fails the build instead.

### The whole C23 language is in scope

There is no dialect subset here and no compatibility shim layer. Any C23 feature
clang and gcc both implement may be used directly: `nullptr`, `enum : uint8_t`,
`constexpr`, `auto`, `[[attributes]]`, bare `static_assert`, `typeof`,
`bool`/`true`/`false`, `_BitInt`, binary literals, digit separators, `alignas`,
`unreachable`, and the `{}` empty initialiser.
The tree reaches for whichever of them states the intent most directly, and the
only thing that constrains the choice is that both compilers in
[`../.github/workflows/ccfish_parity.yml`](../.github/workflows/ccfish_parity.yml)
accept it.

**That rules out a deductive verifier as a gate**, and it is worth saying why so
the question is not reopened. A source-analysing prover has to parse the tree
with its own front end, and no current one parses this dialect: Frama-C 33.0~beta
— the newest release — rejects 9 of the 14 C23 features above, accepting only
`bool`/`true`/`false`, `typeof`, binary literals, `alignas` and `unreachable`.
Several of the rejects have no workaround at all: `auto`, digit separators and
`[[attributes]]` are lexical and type-inference features, so no macro can stand
in for them. Adopting such a tool means writing the engine in the subset it
parses.

For a bit-exact clone that trade is the wrong way round, because the
specification is already written down: **"identical to Stockfish"**. It is
checked directly and end to end by `./build.sh upstream-parity` against a
pristine upstream build, by `./build.sh perft` as a total check on move
generation, by the property suite under ASan+UBSan, and by the signature and
golden-diff gates — all described in [07-tooling-ci.md](07-tooling-ci.md). Those
assert the property that actually matters here, over the whole engine rather than
over leaf helpers.

### Enums with a fixed underlying type

[`../src/engine/board/types.h`](../src/engine/board/types.h) writes every value
type as `typedef enum : uint8_t` (or `: int8_t` for `Direction`), so the width is
**stated, not inferred**. Two things depend on it:

- Struct layout. `Position` and `StateInfo` hold these types directly; an
  implementation-chosen `int` would change their size and their cache behaviour
  without changing a line of source.
- Table indexing. Each type's named bound (`SQUARE_NB`, `PIECE_NB`, `MAX_MOVES`,
  `MAX_PLY`) sizes the arrays it indexes, so the width and the bound are one fact
  written twice. Widen one without the other and the extra values are in range
  for the type and past the end of every table — see
  [01-engine-board.md](01-engine-board.md).

### `sq_add` / `sq_sub`

Enum-to-enum arithmetic is what `-Wconversion` exists to catch, and squares are
added to directions constantly. The two helpers make the intended narrowing
explicit in exactly one place:

```c
static inline Square sq_add(Square s, Direction d) { return (Square) ((int) s + (int) d); }
static inline Square sq_sub(Square s, Direction d) { return (Square) ((int) s - (int) d); }
```

The invariant they do **not** carry is the interesting one: neither checks that
the result is on the board. `NORTH_EAST` from H4 produces an in-range index that
is geometrically wrong, which is why `safe_step` in
[`../src/engine/board/attacks.c`](../src/engine/board/attacks.c) guards on file
distance rather than on index range. Use `sq_add` to silence the conversion
warning; use `safe_step` when the step may leave the board.

Do not open-code `(Square)(s + d)`. It compiles, it warns, and the warning is the
only thing standing between a wrap and a silent out-of-bounds table index.

### `nullptr`, designated initialisers, `static inline`

- **`nullptr`** is the null pointer constant everywhere — the injected output sink
  in [`../src/engine/search/search.c`](../src/engine/search/search.c), the TT
  pointer in [`../src/engine/search/tt.c`](../src/engine/search/tt.c), the
  `strtok` continuation calls in [`../src/shell/uci.c`](../src/shell/uci.c). Not
  `NULL`, not `0`. `nullptr` has a type, so passing it where an integer was meant
  is a diagnostic rather than a surprise.
- **Designated initialisers** carry every aggregate with more than two fields —
  the `TTData` and `TTProbeResult` returns in
  [`../src/engine/search/tt.c`](../src/engine/search/tt.c) are the clearest case,
  where a positional initialiser would be a silent reordering bug the moment a
  field is inserted between `depth` and `bound`.
- **`static inline` in headers** is how the hot leaf functions are shared:
  `lsb`, `msb`, `pop_lsb`, `popcount_bb`, `shift_bb` in
  [`../src/engine/board/bitboard.h`](../src/engine/board/bitboard.h), and the
  square/piece accessors in `types.h`. There is no LTO in the build, so a
  cross-translation-unit call in the recursion is a real call. `static inline`
  puts the body where the optimiser can see it.

  The cost is that the body is **in the header**, so every translation unit that
  includes it is recompiled against the new body. `build.sh` does not track header
  dependencies, so run `./build.sh clean` after editing one of these; a partial
  rebuild links two versions of the same inline function.

## The warning set

```
-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
-Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
```

Set once in `CFLAGS_COMMON` in [`../build.sh`](../build.sh) and applied to every
step — release, debug, zone-check, and the test binary — so a warning cannot hide
in a configuration nobody builds.

| Flag | What it buys here |
| --- | --- |
| `-Wconversion` / `-Wsign-conversion` | The load-bearing pair. The engine mixes `uint8_t` enums, `int` scores, `uint64_t` bitboards and `int16_t` history entries; an implicit narrowing between them is a wrong number, not a crash. This is also the flag that makes `sq_add` necessary. |
| `-Wshadow` | The recursion nests `alphabeta` frames with near-identical local names; a shadowed `depth` or `alpha` reads correctly and searches the wrong tree. |
| `-Wstrict-prototypes` / `-Wmissing-prototypes` | `()` is not `(void)` in a pre-C23 reading, and a function with no prototype in a header is a function nothing checks the arguments of. Together they force every non-`static` symbol to be declared in a header the caller includes. |
| `-Wno-unused-parameter` | The one suppression. Seam signatures carry parameters a given implementation ignores; the alternative is a `(void)x;` line per function, which is noise that hides the real cases. |

Warnings are not errors in `build.sh`. The gate is the human and the review, plus
the gcc lane, whose `-Wconversion` and `-Wshadow` findings differ from clang's and
therefore surface sloppiness clang happens not to diagnose.

## Why there is no build system

`build.sh` enumerates `SOURCES` and `ENGINE_SOURCES` by hand and runs one clang
invocation per step. No Makefile, no CMake, no dependency tracking.

The trade is stated plainly: **a new `.c` file must be added to `SOURCES`, and, if
it lives in `engine/` or `platform/`, to `ENGINE_SOURCES` too.** Forget the second
and `zone-check` and the test binary do not see the file — the release binary
builds, the gates pass, and the module is untested. That is the failure mode this
choice buys, in exchange for a build that has no configure step, no generated
files, no stale-object class of bug, and one place to read the full compile
command.

It also means `clean && build` is the dependency graph. A header edit is not
tracked; rebuild from scratch after touching one.

**The port will strain this.** The port map lists on the order of a hundred and
thirty modules, and the two hand-maintained lists are the thing that scales worst.
Revisit the decision when adding a file is the step people forget — not before,
and not by adding a generator that hides which files are in the engine zone.

## Translating Zig to C23

zfish has already made the structural decisions C cannot express — no templates,
no classes, no RAII, no operator overloading. What is left is a set of recurring
mechanical translations. Each one below has a failure mode that shows up as a
wrong node count rather than as a compile error.

### Explicit casts become implicit conversions

Zig has no implicit integer conversion: `@intCast`, `@truncate`, `@as` and
`@intFromEnum` mark every width or signedness change at the site. C converts
silently. **`-Wconversion` and `-Wsign-conversion` are the tree's replacement for
that property**, and they are why those flags are non-negotiable.

The translation rule: a Zig `@intCast` becomes a C cast **written out**, not
dropped because the compiler would have done it anyway. Dropping it compiles and
warns; the warning is the only record that a narrowing was intended.

`@truncate` and `@intCast` are not the same thing and must not both become a plain
cast in your head. `@intCast` asserts the value fits (it is a checked narrowing in
Zig's debug modes); `@truncate` deliberately discards high bits. In C both spell
`(uint8_t) x`, so the distinction survives only in a comment — write it where
upstream relies on the truncation, as `tt_store`'s `depth8` does.

### Wrapping is opt-in in Zig and undefined in C

Zig spells wrapping arithmetic `+%`, `-%`, `*%`; plain `+` traps on overflow.
Upstream Stockfish relies on wrapping in places, so those operators appear in
zfish exactly where it does.

In C, **signed** overflow is undefined behaviour and **unsigned** wraps. A `+%` on
a Zig signed type therefore cannot be translated as a C signed `+`: do the
arithmetic in the matching unsigned type and convert back, or widen. `stats_update`
in [`../src/engine/search/history.c`](../src/engine/search/history.c) is the live
example of the general hazard — the gravity term exists so an `int16_t` entry
cannot overflow, and deleting it invokes UB that only a deep enough search reaches.
The unsigned-borrow requirement in `entry_relative_age` in
[`../src/engine/search/tt.c`](../src/engine/search/tt.c) is the same hazard from
the other side: the generation counter must wrap, so the subtraction is written in
an unsigned type on purpose.

The gcc lane is the gate that catches this class: two conforming compilers must
produce the same node count, and a signature difference between them is UB the
optimisers exploited differently. It is never "expected compiler variation".

### Slices become pointer + length

A Zig `[]T` carries its length. In C, pass the pointer and the length as two
parameters, or return an end pointer and treat it as a half-open range — which is
what the move generators already do: `generate` appends at `list` and returns the
new end, and the caller's count is `end - list`.

Pick one convention per API and keep it. The hazard is that a C caller can lose
the length and nothing says so: `generate` does **not** bounds-check, so `list`
must have room for `MAX_MOVES` and only the caller's declaration says it does.
That hazard exists precisely because the slice's length was dropped in
translation, and nothing in the C type system restores it: say the required
capacity in the header, as [`../src/engine/board/movegen.h`](../src/engine/board/movegen.h)
does, and declare every buffer `ExtMove list[MAX_MOVES]`.

### `packed struct` becomes explicit bit manipulation

Zig's `packed struct` has a guaranteed layout and a guaranteed bit order. A C
bitfield has **neither** — allocation order, straddling, and the signedness of a
plain `int` bitfield are all implementation-defined. A struct that is bit-exact
under clang and reordered under gcc is a node-count divergence with no diagnostic.

So: translate a `packed struct` into an integer plus named shift/mask accessors.
The 16-bit `Move` in `types.h` and `gen_bound8` in `tt.h` are both this pattern,
and both keep the layout in one commented place:

```c
type << 14 | (promo - KNIGHT) << 12 | from << 6 | to
```

Do not use C bitfields for anything whose layout is observable.

### `comptime` becomes a macro, a `static inline`, or a runtime-built table

zfish computes tables at comptime. C23 has no equivalent, and there are three
landing places, in order of preference:

1. **A `static inline` function**, when the Zig was a comptime-generic helper over
   one or two types. It type-checks; a macro does not.
2. **A table filled at startup.** `bitboards_init` and `position_init` build
   `PseudoAttacks`, `BetweenBB`, `LineBB` and the Zobrist keys this way. The cost
   is the init-order constraint described in
   [00-architecture.md](00-architecture.md) — the tables are zero, not garbage,
   before the call, so the failure mode is a silent no-attacks board.
3. **A macro**, only when neither of the above will do. A macro that evaluates an
   argument twice is a bug in a tree where arguments are `pop_lsb(&b)`.

Generating a `.c` file offline and committing it is the fourth option and needs a
gate that regenerates and diffs it, or the generator rots away from its output.

### Error unions become return codes

Zig's `!T` forces the caller to handle the error. C does not, so the convention
has to be carried by hand and stated at the declaration:

- **`bool` plus an out-parameter** where the failure is expected and local:
  `pos_set` returns `false` and leaves `pos` unspecified; `tt_resize` returns
  `false` on allocation failure.
- **A sentinel** where the type has a spare value: `MOVE_NONE` from
  `move_from_uci`, `VALUE_NONE` from a TT miss.

The rule the sentinel form needs is that **the sentinel must be unrepresentable as
a real value**. `MOVE_NONE` works because no legal move has `from == to`. A
sentinel that a valid computation can produce is a bug that looks like data.

Whichever form, say in the header what the object's state is after a failure.
`pos_set` leaving `pos` *unspecified* on `false` is the contract; a caller that
keeps using it is the defect, and only the header can say so.

## Measurement discipline

The port is allowed to be slow. It is not allowed to be a guess.

**Never quote a number this repo computes.** Not the bench signature, not a node
count, not an nps figure, not how many modules are ported. `./build.sh signature`
and `./build.sh port-status` print the current values; a number written into prose
is wrong the next time it moves and nobody greps the docs for it.
`./build.sh docs-lint` fails on a quoted signature, and only on that one.

**A performance claim ships with the command that produced it.**

```bash
./build.sh bench 8      # Total time / Nodes searched / Nodes/second, on stderr
```

Record `Nodes/second` before and after, on an idle machine, and quote both. The
**node total must not move**: if it does, the change altered behaviour, not speed,
and `./build.sh signature` will say so before you do.

**A behaviour claim ships with a gate.** `./build.sh parity` is the aggregate. A
gate whose tool is missing exits 127 and is a *skipped* gate — `parity` names each
one it skipped, and a run with skips proves less than a clean one.
