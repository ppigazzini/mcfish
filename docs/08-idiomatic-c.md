# Idiomatic C23 here

The C23 the tree commits to, the warning set that enforces it, why there is no
build system, and — because it is the daily work of this repo — the recurring
patterns for expressing upstream's C++ constructs in C23.

Audience: hot-path and build contributors. The gates that hold these rules are in
[10-tooling-ci.md](10-tooling-ci.md).

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

### Which of them this tree actually uses, and which it refuses

"In scope" is not "adopt everything". A feature earns its place by stating an
intent more directly or by closing a defect, and three below are refused on
measurement rather than taste. The verdicts are the result of walking the whole
feature set against the tree, and each was checked by compiling — on clang,
gcc-13 and gcc-14 — rather than by reading a support table.

| feature | verdict | why |
|---|---|---|
| `nullptr` | **used** | the null pointer with a type |
| `enum : T` | **used** | the newtype tier; see [09-type-design.md](09-type-design.md) |
| `constexpr` | **used** | every constant table; refuses a runtime initialiser |
| `[[nodiscard]]` | **used** | enforced as an error, not advisory |
| `[[maybe_unused]]` | **used** | the escape hatch for a fixed callback signature |
| `[[fallthrough]]` | **used** | an intentional switch fallthrough |
| bare `static_assert` | **used** | layout and width contracts |
| `alignas` / `alignof` | **used** | no `<stdalign.h>` behind them |
| `bool`/`true`/`false` | **used** | keywords; `<stdbool.h>` deleted |
| `stdc_count_ones` | **used** | identical codegen to the builtin, at every tier |
| `stdc_bit_ceil_ull` | **used** | replaced a hand-rolled guard + `clz` + shift, identical codegen |
| `[static N]` | **used** | on the only fixed-extent array parameters here |
| `stdc_trailing_zeros` / `stdc_leading_zeros` | **refused** | defined at zero, which costs one instruction per call at sse41 on `pop_lsb`; the precondition already excludes zero |
| `unreachable()` | **refused** | it converts a reachable bug into undefined behaviour; the engine prefers a wrong answer it can gate over UB it cannot |
| `[[unsequenced]]` / `[[reproducible]]` | **refused** | no compiler here implements them — clang and both gccs warn and ignore |
| `_BitInt(N)` | **refused** | nothing here needs a width the fixed-size types lack, and it changes promotion rules under a bit-exact anchor |
| `#embed` | **refused** | the net and the tablebases are runtime inputs by design |
| `auto` | **refused** | these files are read side by side with upstream's C++; an inferred type costs the reader the comparison |
| `typeof` / `_Generic` | **not applicable** | the one rounding macro is used in enum initialisers and `static_assert`s, which no inline function can supply — C has no `constexpr` FUNCTION. Nothing else has a double-evaluation hazard |
| `<stdckdint.h>` | **not applicable**, re-checked | see below |
| binary literals, digit separators, `{}` empty init | **available, unused** | conforming on all three (checked with `-pedantic-errors`); no site currently reads better for them |

**Why there is no checked arithmetic here**, since "we parse untrusted files and
never call a checked-arithmetic macro" is the kind of claim that should look wrong
until it is explained. The parsers **refuse before they compute**, rather than
computing and then detecting overflow, which is the stronger of the two patterns:

- `decode.c` rejects a block or span log `>= 64` before `1 << log`, so a raw file
  byte cannot drive the shift out of range; it rejects an inverted or oversized
  symbol-length pair before the width arithmetic; and `base64_size` is therefore
  `<= 63` and `symlen_size <= 65535` by construction, so neither the
  `* sizeof(uint64_t)` nor the `+ 1` can wrap.
- `nnue_parse.c` checks `blob_len` against the header size *before* subtracting
  it, so the length cannot underflow to a huge `size_t`.
- Every spin option is range-validated against its own min and max before it
  reaches `tt_resize`, so the megabyte-to-bytes multiply is bounded by the option
  table rather than by the value a GUI sent.

That audit covers shifts and subtractions, not only multiplications. The first
pass looked at `alloc(a * b)` sites alone and would have missed both of the first
two. A checked-arithmetic macro would add a second check after a value that is
already refused, which is ceremony rather than safety.

The two refusals worth remembering are the shape of the rule: `stdc_count_ones`
and `stdc_trailing_zeros` arrived in the same header on the same day, and one is
free while the other is not. Only disassembly separates them.

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
golden-diff gates — all described in [10-tooling-ci.md](10-tooling-ci.md). Those
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
  square/piece accessors in `types.h`. The release build **is** `-flto`
  (`CFLAGS_RELEASE`), but that does not make the boundary free: link-time
  optimisation folds constants across translation units and inlines under size
  budgets, and it leaves most out-of-line calls out of line — so a leaf called once
  per node from another unit is still a real call. `static inline` puts the body
  where the optimiser can see it without needing that judgement to go your way.
  This is the same mechanism the file-split rule below rests on.

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
it lives in `engine/` or `platform/`, to `ENGINE_SOURCES` too.** Forgetting the
second used to be silent — the release binary built, the gates passed, and the
module was untested; `zone-check` now compares both arrays against the tree and
names the file. That is the failure mode this
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

### Split files at the cold seam; keep hot bodies in headers

Split a long file where its cold code is: parsers, table builders and init
paths move to their own translation unit whenever that clarifies ownership.
[`nnue_acc_rowops.c`](../src/engine/eval/nnue/nnue_acc_rowops.c) is the model —
kernels split out of the accumulator, and every one of them folds back into its
caller at link time.

Keep a hot-path body in a header, as `static inline` over its extern state. The
invariant that forces this: a function that survives as a symbol in the release
binary is entered by a real call on every use — link-time optimisation here
folds constants across translation units but keeps out-of-line calls out of
line. `tt_probe` in [`tt.h`](../src/engine/search/tt.h) and the per-node seams
in [`search_common.h`](../src/engine/search/search_common.h) own the pattern:
each moved from a `.c` file into its header, and each move carried a measured
instruction win recorded in its commit.

Prove every split on the instruction axis before committing it, in either
direction: `nm build/mcfish` answers whether a body inlined, and
`tools/perf_counters.sh` answers what the move cost. Judge a file's length by
its cold lines — setup code that has outgrown its neighbours wants a new file;
one long specialized hot body is the shape the measurements chose.

The mechanism, so the rule survives its author: link-time optimisation inlines
across translation-unit boundaries under explicit size budgets and skips most
other cross-module optimisations — a boundary is cheap for cold code and a real
optimisation fence for hot code ([11-references.md](11-references.md),
"Translation units, LTO and layout"). Keep per-file translation units and solve
hot visibility with header bodies: a merged-unit build and global
inline-threshold flags both trade this tree's selective control for a blunt
global knob, and the measured comparison gives them nothing in return. The
health check is standing: every hot function still present as a symbol in the
release binary must have a counterpart symbol in the upstream build — a hot
symbol upstream inlines and mcfish does not names the next seam to move.

### A runtime flag where upstream has a template parameter costs real work

Upstream instantiates `search<NodeType>`, `generate_all<Us, Type>` and
`update_piece_threats<ComputeRay>`; the naive C port carries the same choice as a
runtime argument through one shared body. The tell is mechanical: if the function
survives as a symbol in a profile, the constant never reached its branches. The
paying translation is an `always_inline` implementation taking the flag as a
parameter, cloned by thin per-literal entry functions that LTO folds — measured
three times, largest wins of their campaigns. The mirror rule also held every
time it was tested: once the body IS the specialized clone, leave it monolithic.
Outlining any in-body block — even one that never executes — regresses, because
the helper un-folds the constants and perturbs register allocation.

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
`tt_save`'s `depth8` does.

### Wrapping is undefined on signed types

Upstream Stockfish relies on wrapping arithmetic in places, and C gives it to you
on unsigned types only.

In C, **signed** overflow is undefined behaviour and **unsigned** wraps. Wrapping
arithmetic on a signed type therefore cannot be written as a C signed `+`: do the
arithmetic in the matching unsigned type and convert back, or widen. `stats_update`
in [`../src/engine/search/history.c`](../src/engine/search/history.c) is the live
example of the general hazard — the gravity term exists so an `int16_t` entry
cannot overflow, and deleting it invokes UB that only a deep enough search reaches.
The unsigned-borrow requirement in `tt_entry_relative_age` in
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

C does not force a caller to handle an error. The convention is stated at the
declaration, and **`[[nodiscard]]` is what makes the compiler hold callers to it** —
applied to every declaration whose return value reports failure, in both forms
below. Both compilers in the parity workflow honour it, and both accept the
`(void)` cast as the deliberate-discard escape the tree already uses.

It is deliberately **not** on predicates: `pos_legal`, `see_ge` and
`search_stopped` return an answer, not an error, and nothing is carried by hand
there. `worker_ensure_network` reads like the same shape and is not — its `bool`
says a net is resident, and both call sites correctly ignore it because the
classical fallback covers the no-net case.

The two forms:

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
| `_mm_load_si128` where the load is a legacy-SSE operand | `_load_a` / `_store_a`, the aligned forms in [`simd.h`](../src/engine/eval/nnue/simd.h) |
| `_mm_cvtsi32_si128` / `_mm_cvtsi128_si32` | a cast between a scalar and a 1-lane vector, or `v[0]` |

The third row is the exception, and the only one: `__builtin_memcpy` lowers to
`movdqu`, which a legacy-SSE instruction cannot fold as a memory operand. That is
an sse41 concern with a measured price — see *[Alignment has to be
provable](#alignment-has-to-be-provable-not-merely-true)*.

**Constants and reinterpretation.** Free — type-level, no instruction:

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm256_setzero_si256` | `(V) { 0 }` |
| `_mm512_set1_epi8` / `_epi16` / `_epi32` | `(V) { 0 } + x` — the lane type comes from `V` |
| `_mm256_castsi256_ps`, `_mm256_castsi256_si512` | `(Dst) v`, a cast between equal-width vector types |
| `_mm256_extracti128_si256`, `_mm512_inserti64x4` | `__builtin_shufflevector` with constant indices — used in [`simd.h`](../src/engine/eval/nnue/simd.h) to split/reorder lanes with no separate mask register |

**Arithmetic.** The plain C operators are lane-wise on a vector type:

| upstream C++ | mcfish C23 |
| --- | --- |
| `_mm256_add_epi16` / `_epi32`, `_mm256_sub_epi16` / `_epi32` | `a + b`, `a - b` |
| `_mm256_mullo_epi16` | `a * b` |
| `_mm_min_epi16` + `_mm_max_epi16` (ClippedReLU) | `NNUE_VEC_MIN` / `NNUE_VEC_MAX` |
| `_mm_madd_epi16`, `_mm_maddubs_epi16`, `_mm512_dpbusd_epi32` | `nnue_dot_step` — see below for where mcfish keeps per-ISA intrinsics rather than the portable vocabulary |

**There are no saturating operators.** C has no `+|`, and the vector extensions
add none. Upstream's `_mm_adds_epi8` and the `_mm_packs_*` family saturate in
hardware; mcfish reaches the same values by clamping with `NNUE_VEC_MIN`/`MAX`
before a narrowing `__builtin_convertvector`. Writing the plain `+` where
upstream saturates is a **silent correctness change**, not a slow path — and the
place it genuinely matters most is `nnue_dot_step`, where `pmaddubsw` saturates
its int16 intermediate and the scalar body cannot. That the two agree is an
argument (activations are capped at 127, weights are int8, so the pair sum peaks
at 32512), and `simd-scalar` is what checks the argument.

`nnue_dot_step` is not the only place raw per-ISA intrinsics survive, just the
first. The transform's 512-bit packus/pack step (`nnue_accumulator.c`,
`TRANSFORM_PACKUS_BITS == 512`) and the AVX-512 non-zero-index expansion
(`nnue_affine.c`'s `affine_nnz_expand`) both stay outside the portable
vocabulary for the same class of reason — genuine saturation or genuine lane
reordering the portable form cannot express — and both are proven equivalent to
the scalar path the same way: a written correctness argument plus
`simd-scalar`/`arch-determinism`, not a shared C expression. Treat any kernel
this section doesn't name as the default (portable `simd.h` vocabulary); these
three are the named exceptions, not a closed set — grep for raw `_mm`/`__m512`
symbols under `src/engine/eval/nnue/` before assuming there are no others.

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
| `_mm_unpacklo_epi8`, `_mm_shuffle_epi32` | `__builtin_shufflevector`, same as above |

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

## Port upstream's ISA-GATED paths, not just its logic

Upstream does not have one implementation of the hot board and picker code — it has
one per ISA tier, selected by `#ifdef`, and **a port that transcribes only the
portable path silently ships upstream's oldest algorithm at every tier.** That is not
a micro-optimisation left on the table; it was the single largest divergence this
tree has had, and it hid behind every behavioural gate for the port's whole life,
because a different algorithm producing the same attack set produces the same tree.

The board/search-side ones found so far, all now ported, with the gate each is
under here — **this table is not a closed set**, it is the record of instances
found by grepping upstream's gates, and the NNUE zone has already produced two
more of the same class (below the table):

| upstream | gate | what it replaces |
| --- | --- | --- |
| dual hyperbola quintessence (`attacks.h:91`) | `__AVX2__` | magic bitboards — 841 KiB of random-access tables becomes 3 KiB of L1-resident structs, and both ray sets come out of one pass |
| `MoveSorter` (`movepick.cpp:66`) | `__AVX512F__` | the scalar insertion sort's leading run |
| `write_multiple_dirties` (`position.cpp:1157`) | `__AVX512VBMI__` + `VBMI2` | the scalar dirty-threat loop, in the hottest board function there is |

Two NNUE ports fit the identical pattern — upstream reaches a narrower-tier body
from a generic `#else` arm, and a naive port ships that body at every tier:

| upstream | gate | what it replaces |
| --- | --- | --- |
| `FeatureTransformer::transform`'s packus body (reached by the generic arm at every x86 tier) | `__AVX512BW__` | the portable widening step the two 512-bit tiers were otherwise left on, in `nnue_accumulator.c`'s `TRANSFORM_PACKUS_BITS == 512` arm |
| the NNZ index list (upstream never builds a bitset at `USE_AVX512`) | `__AVX512F__` (+ `__AVX512VBMI2__` for the wide compress) | the bitset walk every other tier correctly uses, replaced by `nnue_affine.c`'s `affine_nnz_expand` |

A contributor porting more NNUE code should grep it for this bug class too — it
is not board/search-exclusive, just where it was found first.

Three rules, each paid for:

- **Find them by grepping upstream for its gates**, not by reading the portable path.
  `grep -hoE "#(if|ifdef|elif) +(defined\()?[A-Z_0-9]+" ` over the board and search
  files lists every one in a few seconds. Two of the three above sit in files this
  port had already "finished".
- **`arch-determinism` is what makes them safe to land.** It builds every tier the
  host can execute and requires one node count, so it compares the vector path
  against the scalar path *on the same tree* — a stronger check than any measurement,
  and the only one that can catch a wrong attack set or a wrong threat list. Run it
  on every such commit; `signature` alone tests one tier.
- **A divergence from upstream is a strong PRIOR, not a proof.** Upstream's
  `Move*`-generator interface plus its vectorised move splats was ported in full,
  bit-exact and fully gated, and measured **slower on three separate runs** — the port
  generated straight into `ExtMove` in one pass where upstream's shape needs two, and
  the splats did not recover the copy. It was reverted. Port them one at a time and
  let the clock rule on each.

## The C23 spellings that measured

Each row is a language-level edit with **no algorithmic content** — the same
arithmetic over the same tree, an identical node count on both sides — that moved
the whole-binary instruction count under `tools/perf_counters.sh` paired rounds at
`bench 16 1 13`. These are the levers to reach for before rewriting anything; the
commit that landed each one carries its rounds, its tiers and, where the shape was
the point, its disassembly. Search the log before re-deriving one.

**The tier is part of the claim.** The two largest entries are sse41-only: VEX and
EVEX encodings fold an unaligned memory operand, so above sse41 the aligned and
the portable spelling compile to the same binary, byte for byte.

`native` in the *measured* column is not a tier: it is whichever of the five the
measuring host selected, so a cell reading `native` is a number about that box's
widest tier and carries to another only if that host selects the same one. See
[the arch ladder](10-tooling-ci.md#the-arch-ladder-and-why-native-is-a-selector),
and re-measure rather than assume — a win at one tier can be flat or negative at
another, which is why each row names the tier at all.

| spelling | owner | mechanism | measured |
| --- | --- | --- | --- |
| `static inline` body in a header, over extern state | `tt_probe` in [`tt.h`](../src/engine/search/tt.h), the per-node seams in [`search_common.h`](../src/engine/search/search_common.h) | *[Split files at the cold seam](#split-files-at-the-cold-seam-keep-hot-bodies-in-headers)* | native −0.29% and −0.33%, super-additive once every seam is inline |
| `always_inline` implementation behind thin literal-argument clones | `search_node_impl` in [`search_main.c`](../src/engine/search/search_main.c), the four entries in [`movegen.c`](../src/engine/board/movegen.c), `threats_update_piece_impl` in [`threats.c`](../src/engine/board/threats.c) | *[A runtime flag where upstream has a template parameter](#a-runtime-flag-where-upstream-has-a-template-parameter-costs-real-work)* | native −1.11%, −1.69%, −0.42% |
| call the clone, not the tag dispatcher | `search_node_nonpv` / `search_node_pv` | clang declines to fold the dispatch layer even at a literal call site | native −0.41% |
| `alignas(CACHE_LINE_SIZE)` on a buffer a legacy-SSE kernel reads | `fc0_out`, `fc1_out`, `concat` in [`nnue_inference.c`](../src/engine/eval/nnue/nnue_inference.c) | `pmaddubsw` folds an aligned memory operand and cannot fold `movdqu` | sse41 −3.60%; avx2 and native unmoved |
| `_load_a` / `_store_a` instead of `__builtin_memcpy` | [`simd.h`](../src/engine/eval/nnue/simd.h), used across [`nnue_acc_rowops.c`](../src/engine/eval/nnue/nnue_acc_rowops.c) | the same folding, on the row kernels' `psubw`/`paddw` | sse41 −4.06%; identical binaries above it |
| one cache-line-aligned block per attacker, not three separately based tables | `ThreatIndexBlocks` in [`nnue_feature.c`](../src/engine/eval/nnue/nnue_feature.c) | `nnue_full_make_index` pays one block base and two loads | native −1.16% |
| field **order** in a hot struct | the bool cluster in `RootMove`, the per-node block in `SearchCtx` ([`search_types.h`](../src/engine/search/search_types.h)) | interleaving the bools kills an SLP mask-domain lowering; keeping the per-node scalars contiguous keeps them off three further cache lines | native −0.22% |
| a hot scalar by value in the context, not behind a pointer | `time_state` in `SearchCtx`, `histories_bind_shared` in [`history.h`](../src/engine/search/history.h) | a dependent double-load chain becomes one flat read — a decrement-and-branch in `check_time`'s case | native −0.08% and −0.21% |
| parameters narrowed to what the callee actually reads | `search_update_continuation_histories` | the caller passes fields straight off its stack, with no gather to build | native −0.14% |
| a non-null fallback singleton instead of a null test | the one-cluster fallback in [`tt.c`](../src/engine/search/tt.c) | probe, prefetch and save carry no null test, because a table always exists | native −0.06% |
| block scope around a large local | the picker in [`search_qsearch.c`](../src/engine/search/search_qsearch.c) | stack colouring overlays the slot with the buffer that follows it | the frame carries one picker, not two; instructions flat — the win is per-ply stack stride |
| a plain-typed view over `_Atomic` storage in a provably exclusive phase | `history_clear` | *[An `_Atomic` store silently de-vectorizes a hot loop](#an-_atomic-store-silently-de-vectorizes-a-hot-loop)* | figures in that section |
| align the arena **payload**, not the mapping | `page_alloc_default` in [`memory.c`](../src/platform/memory.c) | a size header past the mmap base destroys 2 MiB alignment, and transparent huge pages need an aligned start | vnni512 PGO, spine harness intra-engine 0.955 |

None of them is allowed to move behaviour: `./build.sh signature` holds the anchor
and `./build.sh arch-determinism` holds every tier the host can execute, which is
what makes an alignment or field-order edit safe to land at all. Every figure
above is the instruction axis alone — what that axis can and cannot settle is
*Measurement discipline* below.

### Alignment has to be provable, not merely true

A buffer that happens to be 64-byte aligned buys nothing. The compiler folds a
legacy-SSE memory operand only where the **type** carries the alignment, so the
promise is made in the source: `alignas` on the storage, and a load through a
typedef that states it. `NNUE_SIMD_ALIGN_CAP` in
[`simd.h`](../src/engine/eval/nnue/simd.h) caps that claim at the arena's 64-byte
guarantee — a `vector_size` type's natural alignment is its full width, which no
arena offset provides, so claiming it would be a lie the sanitizers eventually
collect.

Use `_load_a` / `_store_a` where the pointer is a multiple of `NNUE_ALIGN` off
64-byte-aligned storage, and the plain `_load` / `_store` everywhere else. Both
are correct; only the aligned pair folds, and only below AVX.

### Struct field order is codegen

Two independent effects, both measured, both invisible in review:

- **Contiguity of the per-node set.** A large cold member in the middle of the hot
  fields pushes the rest onto further cache lines that every node then touches.
- **Runs of same-typed narrow fields.** Four contiguous bools are an invitation the
  SLP vectorizer accepts, assembling them through AVX-512 mask registers where four
  plain byte stores do the job. Interleaving them with narrow scalars so no run
  exceeds two is what stops the merge forming.

Neither is arguable from the source, so re-measure the whole binary after any
reordering — and expect the effect to change sign between tiers.

### What did not pay

- **`noinline` on the two threat specializations.** Forbidding the inline buys each
  variant its own register allocation and charges a call plus its spills to every
  piece touch in every make. Let both inline, as
  [`threats.h`](../src/engine/board/threats.h) says.
- **The same parameter narrowing on the fourth history gather.** It measures +1.2M
  on an LTO inlining flip where three sibling call sites pay; the quiet-histories
  writer keeps the full gather.
- **Cache-line alignment of the magic table.** The D1 reduction is real and
  deterministic and does **not** convert to cycles. It stands as fidelity with
  upstream — cite it that way, never as nps.

**Outlining the movepick stage setups is NOT MEASURED here, and the sign to
expect is negative.** The two siblings ran the identical change and disagree:
`../rfish ae7766d` marks its three `MovePicker` stage setups `#[inline(never)]`
and measures −0.81% instructions at avx2, and `../zfish 710b5c26` ported it
verbatim, reproduced the mechanism exactly, and measured it **positive** on both
tiers — the frame it was supposed to shrink went 744 → 24 bytes and the
instruction count still rose. The reason does not travel: LLVM had already merged
the three move buffers into ONE frame allocated by a single `sub`, whose
immediate is free at either size, so there was nothing to save and the three
call/ret pairs plus argument setup and reloads are the whole delta. rfish's win
is the ~30-instruction Rust prologue it removed, which this tree does not have
either. That is the same mechanism as the `noinline` entry above, which this tree
DID measure. **Measure it here before taking it, or do not take it** — this
paragraph is a predicted sign, not a result.

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

**Measure every edit whole-binary, on the instruction axis first.**
[`../tools/perf_counters.sh`](../tools/perf_counters.sh) with paired rounds gives a
deterministic, load-immune instruction count; build explicitly before each
measurement. Judge a change by the whole binary's count, never by arithmetic over
the diff: the specialized node bodies shift under register-allocation changes, and
an edit near the transposition table flips link-time inlining, so a local estimate
answers a different question than the binary does.

**But the instruction axis is where this port has been misled most, so bound what it
can tell you.** Three checks, each of which has overturned a conclusion here:

- **Subtract startup, by measurement.** `perf_counters` counts the whole process, and
  startup is engine-dependent — mcfish parses the net in about half upstream's time.
  Against the oracle on the spine, the instruction ratio reads 0.940 whole-process and
  **1.002** once startup is subtracted: the entire apparent lead was the net load.
  [`../tools/perf_delta.py`](../tools/perf_delta.py) does the subtraction on absolutes,
  which is the only place it can be done — the difference of two ratios is not the
  ratio of two differences.
- **Read macro-ops beside instructions.** An x86 instruction is not a unit of work; a
  folded load-op retires as one and dispatches as two. When the two columns disagree,
  the instruction ratio is measuring spelling.
- **An instruction win that buys no cycles is not a win.** It is the D8/D9 mirage, and
  the converse now has its own record: a change can be instruction-neutral, cache-
  better and branch-level and still cost 4% on the clock. Gate on the clock.

**Ask `nps_ab.sh` first, and believe it over the counters.** It reads each engine's
own bench clock, which starts after the `ucinewgame` clear and so contains no startup
at all — nothing to subtract, nothing to get wrong. Three rules inside it, each of
which cost a wrong published number:

- **Alternate which engine runs first.** The second slot in a round runs on a hotter
  core; with a fixed order that bias is systematic and a few-percent effect comes out
  with the wrong sign.
- **Sum over a position set; never median per-position times.** Search sizes vary by
  orders of magnitude, so a median is decided by which positions land in the middle —
  the same binaries read 1.091 at depth 12 and 0.906 at depth 14 that way.
- **Measure the binaries that play the games.** Tier and build mode change the answer:
  one deficit here doubled between sse41-plain and icl-PGO, and a conclusion drawn on
  the first said nothing about the second.

**Prefer removing a component over attributing one.** `perft` is the board zone with
no TT, no histories, no move ordering and no evaluation; `MCFISH_EVAL_MATERIAL=1`
leaves the spine and search running with the network gone. Comparing the same pair
over both localises an effect to a zone in two commands, with no per-function
attribution argument to get wrong. Attribution across two differently-inlined binaries
is void by construction; removal is not.

**Validate a counter before believing it.** A counter opened by name is a hypothesis.
[`../tools/perf_counter_validate.c`](../tools/perf_counter_validate.c) runs two loops
whose bottleneck is known — one dependency chain, one independent-ILP loop — and a
counter that does not respond the way the bottleneck demands does not mean what its
name says on this host. Two conclusions in this tree have died to this; see
[10-tooling-ci.md](10-tooling-ci.md#a-counter-is-a-hypothesis-until-it-is-validated).

**Run the call-count parity test FIRST.** `perf_fingerprint.py compare --calls` is
inlining-immune and answers "do we run Stockfish's algorithm?". On the spine it comes
back exact, symbol for symbol, which retires every "we must be doing extra work"
hypothesis in one command — and it costs nothing to run before a day of profiling
rather than after.

**Take cycle and cache claims to an idle box, floored by an A/A run — and know that
each axis has its OWN floor.** Measure the build against a byte-identical *copy* of
itself, both orientations, reported as the geometric mean of the two so the position
bias cancels. The five axes do not share one floor: instructions is exact, and the
four efficiency axes spread over more than an order of magnitude between the
tightest and the widest, so the same reading is a result on one axis and noise on
another. **Per-axis floors: TBD** — derive them on the box in front of you and
quote them beside any ratio you report. No figure recorded here has survived audit.

**That floor is a property of the box's STATE, not a constant of the host.** The
same control run immediately after a heavy build reads far wider than the settled
one, while instructions stay exact. So run the control *adjacent in time* to the
comparison it floors, and when it reads wide, discard the comparison rather than the
control: a wide A/A is the box saying every efficiency ratio measured beside it is
noise. A four-tier sweep has been thrown out and re-run for precisely this. A
control taken once at the start of a session floors nothing an hour later.

**Subtract startup before quoting any search ratio.** `perf_counters.sh` counts the
whole process, and net load is a large share of a shallow bench — large enough that
the whole-process ratio and the search-only ratio have been observed to disagree in
SIGN. Measure a near-empty search separately and subtract it, or the number
describes the loader rather than the engine. This is the error that put a false
standing into this page and into the local ledger; treat any un-subtracted
whole-process ratio as unusable.

The two halves of an IPC gap floor differently, which is what makes the split worth
reading: **branch misses move for a prediction change; cache misses move for a data
one.** A repeatable cache-miss win earns a commit only when cycles follow it — this
tree has measured miss reductions that cost more cycles than they saved, in both the
prefetch and the wide-store families. State which axis a commit's evidence rides on
in its body.

**Instructions lead because they are deterministic, not because they predict Elo.**
The obvious test — rank the ISA tiers by each counter axis and by measured Elo
against the same-tier oracle, then correlate — has been run here and settles
nothing, and could not have: with four tiers the Elo ranking it would need to
reproduce is itself sub-sigma. The sibling zfish port ran the same experiment and
landed at the opposite extreme on the instruction axis; two ports reaching opposite
conclusions from n = 4 is what a noise-driven correlation looks like from the
inside. **Do not read a tier ordering out of the counter table, in either
direction**, and do not restate the correlation as a figure — it carries no
information. **Rank correlations: TBD, and likely not worth deriving.**

**A per-function ratio is a lie unless the grouping is inlining-neutral.** callgrind
attributes inlined code to the file it came from under the CALLER's name, and the
two engines inline differently — mcfish folds `do_move` into the node body where
upstream keeps it out of line. Comparing one symbol per side has produced large
fictitious divergences in this tree more than once, in both directions. Group every
logical component with its inlined callees on BOTH sides and let
[`../tools/perf_fingerprint.py`](../tools/perf_fingerprint.py) reconcile the sum
against callgrind's own program totals; it fails loudly on a shortfall where a
hand-rolled parser prints a plausible lie.
