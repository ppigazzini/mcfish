# Idiomatic C23 here

The C23 the tree commits to, the warning set that enforces it, why there is no
build system, and — because it is the daily work of this repo — the recurring
patterns for expressing upstream's C++ constructs in C23.

Audience: hot-path and build contributors. The gates that hold these rules are in
[09-tooling-ci.md](09-tooling-ci.md).

**Path convention on this page.** A Stockfish golden is written relative to
Stockfish's `src/`, as *upstream `nnue/network.cpp`*. That is not a path in this
repository, and writing it bare would make `./build.sh docs-lint` assert it
exists here.

## C23, pinned and probed

[`../build.sh`](../build.sh) selects the C23 flag by **probing the compiler**,
not by pinning a version:

```bash
for f in -std=c23 -std=c2x; do ... done
```

GCC only learned `-std=c23` in 14; GCC 13 accepts the same language as
`-std=c2x`. Probing is what lets the second-compiler lane in
[`../.github/workflows/mcfish_parity.yml`](../.github/workflows/mcfish_parity.yml)
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
[`../.github/workflows/mcfish_parity.yml`](../.github/workflows/mcfish_parity.yml)
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
golden-diff gates — all described in [09-tooling-ci.md](09-tooling-ci.md). Those
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

## Porting patterns

C cannot express templates, classes, RAII or operator overloading, and upstream
uses all four. What is left after removing them is a set of recurring mechanical
translations. Each one below has a failure mode that shows up as a wrong node
count rather than as a compile error.

### Every width or signedness change is written out

C converts integers silently, which is exactly how an unintended narrowing
survives review. **`-Wconversion` and `-Wsign-conversion` are the tree's guard
against that**, and they are why those flags are non-negotiable.

The rule: a narrowing is a C cast **written out**, not dropped because the
compiler would have done it anyway. Dropping it compiles and warns; the warning is
the only record that the narrowing was intended.

An assertion that a value fits and a deliberate discard of high bits are not the
same operation, and both spell `(uint8_t) x` in C — so the distinction survives
only in a comment. Write it wherever upstream relies on the truncation, as
`tt_store`'s `depth8` does.

### Wrapping is undefined on signed types

Upstream Stockfish relies on wrapping arithmetic in places, and C gives it to you
on unsigned types only.

In C, **signed** overflow is undefined behaviour and **unsigned** wraps. Wrapping
arithmetic on a signed type therefore cannot be written as a C signed `+`: do the
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

### Buffers are pointer + length

Pass a buffer as a pointer and an explicit length, never a bare pointer — or
return an end pointer and treat it as a half-open range, which is what the move
generators already do: `generate` appends at `list` and returns the new end, and
the caller's count is `end - list`.

Pick one convention per API and keep it. The hazard is that a C caller can lose
the length and nothing says so: `generate` does **not** bounds-check, so `list`
must have room for `MAX_MOVES` and only the caller's declaration says it does.
Nothing in the C type system carries that capacity, so state it in the header, as
[`../src/engine/board/movegen.h`](../src/engine/board/movegen.h) does, and declare
every buffer `ExtMove list[MAX_MOVES]`.

### Packed layouts become explicit bit manipulation

A C bitfield has **no guaranteed layout and no guaranteed bit order** —
allocation order, straddling, and the signedness of a plain `int` bitfield are all
implementation-defined. A struct that is bit-exact under clang and reordered under
gcc is a node-count divergence with no diagnostic.

So: express a packed layout as an integer plus named shift/mask accessors.
The 16-bit `Move` in `types.h` and `gen_bound8` in `tt.h` are both this pattern,
and both keep the layout in one commented place:

```c
type << 14 | (promo - KNIGHT) << 12 | from << 6 | to
```

Do not use C bitfields for anything whose layout is observable.

### A compile-time table becomes a macro, a `static inline`, or a runtime build

Upstream computes several tables at compile time. C23 has no general equivalent,
and there are three landing places, in order of preference:

1. **A `static inline` function**, when the upstream form was a generic helper
   over one or two types. It type-checks; a macro does not.
2. **A table filled at startup.** `bitboards_init` and `position_init` build
   `PseudoAttacks`, `BetweenBB`, `LineBB` and the Zobrist keys this way. The cost
   is the init-order constraint described in
   [00-architecture.md](00-architecture.md) — the tables are zero, not garbage,
   before the call, so the failure mode is a silent no-attacks board.
3. **A macro**, only when neither of the above will do. A macro that evaluates an
   argument twice is a bug in a tree where arguments are `pop_lsb(&b)`.

Generating a `.c` file offline and committing it is the fourth option and needs a
gate that regenerates and diffs it, or the generator rots away from its output.

### Errors become return codes

C does not force a caller to handle an error, so the convention has to be carried
by hand and stated at the declaration:

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

## Translate an intrinsic instead of reaching for one

Upstream writes its hot NNUE kernels in x86 intrinsics, one path per ISA. mcfish
writes them **once** in GCC/clang vector extensions
([`simd.h`](../src/engine/eval/nnue/simd.h)) and lets the backend lower them —
AVX-512, AVX2 or SSE on x86, NEON on aarch64. An intrinsic is the last resort,
for the few kernels where the portable form leaves measurable throughput behind.

```c
typedef int16_t V __attribute__((vector_size(16 * sizeof(int16_t))));
V acc = a + b;   // vpaddw on AVX2, vaddw on NEON — the backend's job
```

The evaluation is integer-exact and therefore arch-invariant: every tier must
bench the same number. `./build.sh arch-determinism` runs the real bench on each
ISA the host can execute and asserts they agree, and `./build.sh simd-scalar`
re-asserts the anchor with **every vector type compiled out** — that second gate
is what makes the table below safe to rely on, because a portable spelling that
lowers differently from the scalar body shows up there as a moved anchor rather
than as a wrong evaluation nobody notices.

The mapping worth knowing before touching a kernel.

**Memory.** Alignment is a property of the pointer, not the operation. mcfish
loads and stores through `__builtin_memcpy` into a vector-typed local, which
compiles to a single move and is correct at any alignment:

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm256_load_si256` / `_mm256_store_si256` | `__builtin_memcpy(&v, p, sizeof v)` on an aligned buffer |
| `_mm256_loadu_si256` / `_mm_loadu_si128` | the same expression — there is no separate unaligned spelling |
| `_mm_cvtsi32_si128` / `_mm_cvtsi128_si32` | a cast between a scalar and a 1-lane vector, or `v[0]` |

**Constants and reinterpretation.** Free — type-level, no instruction:

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm256_setzero_si256` | `(V) { 0 }` |
| `_mm512_set1_epi8` / `_epi16` / `_epi32` | `(V) { 0 } + x` — the lane type comes from `V` |
| `_mm256_castsi256_ps`, `_mm256_castsi256_si512` | `(Dst) v`, a cast between equal-width vector types |
| `_mm256_extracti128_si256`, `_mm512_inserti64x4` | `__builtin_shufflevector` with constant indices — **no use in the tree today**; the kernels that would need it keep their intrinsics |

**Arithmetic.** The plain C operators are lane-wise on a vector type:

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm256_add_epi16` / `_epi32`, `_mm256_sub_epi16` / `_epi32` | `a + b`, `a - b` |
| `_mm256_mullo_epi16` | `a * b` |
| `_mm_min_epi16` + `_mm_max_epi16` (ClippedReLU) | `NNUE_VEC_MIN` / `NNUE_VEC_MAX` |
| `_mm_madd_epi16`, `_mm_maddubs_epi16`, `_mm512_dpbusd_epi32` | `nnue_dot_step` — the one place mcfish keeps per-ISA intrinsics |

**There are no saturating operators.** C has no `+|`, and the vector extensions
add none. Upstream's `_mm_adds_epi8` and the `_mm_packs_*` family saturate in
hardware; mcfish reaches the same values by clamping with `NNUE_VEC_MIN`/`MAX`
before a narrowing `__builtin_convertvector`. Writing the plain `+` where
upstream saturates is a **silent correctness change**, not a slow path — and the
one place it genuinely matters is `nnue_dot_step`, where `pmaddubsw` saturates
its int16 intermediate and the scalar body cannot. That the two agree is an
argument (activations are capped at 127, weights are int8, so the pair sum peaks
at 32512), and `simd-scalar` is what checks the argument.

**Comparison produces an integer mask, not a bool vector.** This is the sharpest
difference from upstream's intrinsics and from any language with a real mask
type. A vector comparison in GCC/clang yields a vector of the same width whose
lanes are **all-ones** for true and all-zeros for false — so it is consumed with
bitwise `&` / `|` / `~`, exactly as `NNUE_VEC_MIN` does:

```c
#define NNUE_VEC_MIN(a, b) \
    ((__typeof__(a)) (((a) & ((__typeof__(a)) ((a) < (b)))) \
                      | ((b) & ~((__typeof__(a)) ((a) < (b))))))
```

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm_cmpeq_epi8`, `_mm_cmpgt_epi8` / `_epi32` | `a == b`, `a > b` — result is an all-ones mask |
| `_mm512_cmpgt_epi32_mask` | the same comparison; there is no separate mask register type |
| `_mm256_movemask_epi8` | mask, `&` a lane-bit constant, then reduce with `|` |

**Do not assume a mask's representation beyond all-ones/all-zeros.** The width is
the source vector's, the true value is `-1` in the lane's signed type, and
anything past that is the backend's choice.

**Width conversion.** `__builtin_convertvector` between families of the same lane
COUNT. Widening sign- or zero-extends by the SOURCE element's signedness;
narrowing truncates — which is the C conversion the scalar body writes out, and
is why the two paths agree:

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm_cvtepi8_epi16` (sign-extend widen) | `__builtin_convertvector` to a wider signed lane |
| `_mm_packs_epi16` / `_mm_packs_epi32` (signed saturate) | clamp with `NNUE_VEC_MIN`/`MAX`, then `__builtin_convertvector` |
| `_mm_packus_epi16` (unsigned saturate) | the same, clamped to the unsigned range |
| `_mm_unpacklo_epi8`, `_mm_shuffle_epi32` | `__builtin_shufflevector` — **no use in the tree today**, same reason |

**Shifts.** `v << s` and `v >> s` take a scalar count. Signedness of the LANE
picks the instruction — `>>` on a signed lane is arithmetic (`_mm_srai_epi16`),
on an unsigned lane logical (`_mm_srli_epi16`). That is the whole distinction;
there is no separate spelling, so the lane type is load-bearing.

**Scalar bit operations** keep upstream's builtins: `__builtin_popcountll` for
`popcount`, `__builtin_ctzll` for `_tzcnt_u64`. Both need the ISA flags
`build.sh` already sets — without `-mpopcnt`, `__builtin_popcountll` lowers to a
library call.

## clang auto-vectorizes integer hot loops — so hand-write vectors for a reason

A sibling Zig port of this engine found its NNUE eval carrying a persistent
instruction deficit against upstream, traced to one cause: **its toolchain left
scalar integer loops scalar**, so a `u8 x i8 -> i32` dot compiled to a serial
loop while upstream's Clang build turned the same C++ into `pmaddwd`. Closing that
gap there meant hand-writing a vector form of every such loop.

**mcfish's toolchain does not have that gap, because mcfish is Clang.** The exact
loops that stayed scalar there vectorize here at `-O3`, verified directly:

```c
int32_t dot(const uint8_t *a, const int8_t *w, int n) {
    int32_t s = 0;
    for (int i = 0; i < n; i++) s += (int32_t) a[i] * (int32_t) w[i];
    return s;
}
```

```
$ clang -std=c23 -O3 -msse4.1 -mssse3 -Rpass=loop-vectorize
remark: vectorized loop (vectorization width: 4, interleaved count: 2)
        -> pmaddwd + paddd in the body
```

The clipped-ReLU activation vectorizes the same way (width 8), and both widen on
AVX-512. So do NOT port that sibling's per-loop vectorization slices on the
assumption the compiler needs the help: measure first with
`./build.sh upstream-nodes`-adjacent instruction counting (see *Measurement
discipline*), because the loop is very likely already vector.

**This does not mean stop hand-writing `simd.h`.** The kernels there are explicit
for two reasons the auto-vectorizer cannot serve, and both are about correctness,
not speed:

- **Bit-exactness with the scalar fallback.** `MCFISH_SIMD_SCALAR` builds a
  struct-of-scalars oracle that `./build.sh simd-scalar` asserts is value-identical
  to the vector path. An auto-vectorized loop gives no such second implementation
  to check against, and no guarantee the two agree on a saturating edge.
- **Saturation the auto-vectorizer would get wrong.** `nnue_dot_step` lowers to
  `pmaddubsw`, whose int16 intermediate SATURATES; the plain C `a[i]*w[i]` sum
  does not. They agree only because the inputs are bounded (activations capped at
  127, weights int8, so the pair sum peaks at 32512). Writing the kernel by hand
  is what pins that instruction; leaving it to the vectorizer would pick whatever
  the cost model prefers, which need not saturate identically.

The rule: hand-write a vector kernel when its EXACT lowering is load-bearing —
saturation, the bit-exact oracle, a specific reduction — and let Clang vectorize
the rest. The compiler is not the adversary the Zig port had to work around.

### An `_Atomic` store silently de-vectorizes a hot loop

A store to an `_Atomic` member cannot be vectorized — the loop vectorizer reports
`instruction cannot be vectorized` and emits one scalar store per element. That is
correct (an atomic store has ordering the vectorizer must not reorder), but it
turns a bulk fill or copy over an atomic array into scalar code, and nothing warns
you: the type is right, the loop is right, only the throughput collapses.

This was the single largest instruction gap against upstream. The shared history
tables are `_Atomic int16_t` for concurrent-search safety, and `history_clear`
filled ~4 million of them one atomic store at a time — 183M instructions against
upstream's 67M plain-`int16` clear.

The fix is not to drop the atomicity the search needs, but to bypass it in the
phase that does not: a clear or a resize runs with no concurrent reader, so it
fills through a plain `int16 *` view of the same memory and vectorizes into
broadcast stores. `history_clear` went to 14M — below upstream — and the anchor
and `tsan-search` both held, because the exclusive phase genuinely has no race.

The general rule: **`_Atomic` is for the concurrent phase only.** Where a bulk
operation is provably exclusive (a startup clear, a single-writer reset), cast to
the plain element type and let Clang vectorize; keep the atomic access for the
concurrent path. Grep for the class with
`clang … -Rpass-analysis=loop-vectorize` and look for `instruction cannot be
vectorized` on a fill loop.

## Measurement discipline

The port is allowed to be slow. It is not allowed to be a guess.

**Never quote a number this repo computes.** Not the bench signature, not a node
count, not an nps figure. `./build.sh signature` prints the current value; a number
written into prose is wrong the next time it moves and nobody greps the docs for it.
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
