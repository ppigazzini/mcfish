# References

Links only. Anything a reader could learn from one of these does not belong in
the rest of this set — see [13-writing.md](13-writing.md).

Audience: all developers.

## The repository that defines the work

mcfish is a C23 port of **Stockfish**, which is the reference implementation for
everything here.

- [Stockfish][stockfish] — the **golden**. It defines correct behaviour, and the
  differential gate compares against a pristine upstream build. Where mcfish and
  Stockfish disagree, Stockfish wins.
  - [Bench and the node signature][sf-bench] — the finish line for this port.
  - [Stockfish releases][sf-releases] — the `Bench:` line for a tagged commit.

## Chess programming

- [Chess Programming Wiki][cpw] — the domain reference.
  - [Bitboards][cpw-bb] · [Magic bitboards][cpw-magic] — the slider lookup in
    [`../src/engine/board/attacks.c`](../src/engine/board/attacks.c).
  - [Perft results][cpw-perft] — the reference counts.
  - [Alpha-beta][cpw-ab] · [Quiescence search][cpw-qs] ·
    [Late move reductions][cpw-lmr] · [Null-move pruning][cpw-null]
  - [Transposition table][cpw-tt] · [Zobrist hashing][cpw-zobrist]
  - [Lazy SMP][cpw-lazysmp] — the threading model. The pool is ported into
  `src/platform/`, in the build and driven by the search: `Threads` above 1 runs
  that many workers over one root. See [04-multithreading.md](04-multithreading.md).
- [UCI protocol specification][uci] — the wire protocol.
- [Chess960 / Fischer Random][cpw-960] — the castling encoding the move format
  carries.

### NNUE

- [NNUE pytorch trainer docs][nnue-doc] — the canonical description of the
  architecture, the feature sets, and the quantization.
- [Stockfish NNUE sources][sf-nnue] — the golden for the evaluation port.
- [Leela Chess Zero training data][lc0-data] — what the networks are trained on,
  under the [ODbL][odbl].

### Syzygy tablebases

- [Syzygy tablebases][syzygy] — the format and the probing rules.
- [Stockfish's Syzygy prober][sf-syzygy] — the golden for the tablebase port.

## C23

- [N3220][n3220] — the C23 working draft, the practical reference for the standard
  as published.
- [cppreference: C][cppref-c] — the day-to-day lookup for library and language
  behaviour, with per-version notes.
- [clang diagnostics reference][clang-diag] — what each flag in the warning set
  actually catches.

## Type theory and type design

Background for [09-type-design.md](09-type-design.md). Six groups, each here for
one job; two of them are carried to mark a **limit** rather than to support a
claim, and neither is a claim that this port implements anything.

**What a type denotes.** The frame the design page assumes — a type is a set of
values, and membership is construction.

- [Harper, *Practical Foundations for Programming Languages*, 2nd ed.][pfpl] —
  the textbook treatment of sum types, refinement, and what a type does and does
  not carry.

**Boolean blindness.** The argument for replacing a boolean with a named
alternative, and the one to make when proposing one: a boolean gives 2ⁿ states for
*n* meanings, and branching on it loses what was tested.

- [Harper, "Boolean Blindness"][boolblind] — *"There is no information carried by
  a Boolean beyond its value. To make use of one you have to know its
  provenance."* This is what `NodeType` rests on, and what `cut_node` still
  violates knowingly.

**Units of measure.** The line the clock's two units would need. Kennedy's design
gives *unit polymorphism* — a function generic in the unit it returns — which is
exactly the property a `Depth` type would need and cannot have.

- [Kennedy, "Types for Units-of-Measure"][kennedy] — the reference treatment,
  still the reference. Library-level encodings in other languages give the
  checking and lose the inference.

**Strong typedefs in C specifically.** The thing C does not yet have, and the
reason this port's enum tier is a promoted warning rather than a language rule.

- [WG14 N3320, "strong-typedef"][n3320] — `_Newtype` and the `[[strong]]`
  attribute: a distinct type with the same representation that still propagates
  through arithmetic. **A proposal, implemented by no compiler this port builds
  with.** Watch it; do not plan on it.
- [MISRA C essential type model][misra-essential] — the industrial precedent for
  the same idea enforced by a checker rather than by the compiler.

**The zero-cost-abstraction claim, and where it fails.** Carried as a limit. The
usual argument is that a wrapper compiles away; the measured finding across the
sibling ports is that it compiles away *when the value is carried* and perturbs
register allocation when many instances are live in one large function.

- [Stroustrup, "Foundations of C++"][zero-overhead] — the canonical statement of
  the principle the cost rule qualifies.

**Cost in the type system.** Carried as a limit, with its refutation attached.
This is the research line that would answer "why can't the compiler tell me what
this type will cost".

- [Hoffmann & Jost, "Two decades of automatic amortized resource
  analysis"][aara] — types carry *potential*; the type system performs the
  physicist's method of amortized analysis. **The limit is total:** AARA bounds
  algorithmic resource use — allocations, steps, heap — not register pressure
  inside an inlined function, which is what every regression these ports measured
  actually was. Do not cite it as applicable until it can say something about this
  tree.

[pfpl]: https://www.cs.cmu.edu/~rwh/pfpl.html
[boolblind]: https://existentialtype.wordpress.com/2011/03/15/boolean-blindness/
[kennedy]: https://www.microsoft.com/en-us/research/publication/types-for-units-of-measure-theory-and-practice/
[n3320]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3320.htm
[misra-essential]: https://misra.org.uk/
[zero-overhead]: https://www.stroustrup.com/ETAPS-corrected-draft.pdf
[aara]: https://www.cs.cmu.edu/~janh/assets/pdf/HoffmannJ21.pdf

## Translation units, LTO and layout

Background for the file-split rule in
[08-idiomatic-c.md](08-idiomatic-c.md#split-files-at-the-cold-seam-keep-hot-bodies-in-headers):
link-time optimisation inlines across translation-unit boundaries under explicit
budgets and skips most other cross-module optimisations, which is why a hot body
must live in a header while a cold one may split freely.

- [LTO architecture, cross-TU optimisations and limitations][lto-survey] — the
  survey of what each toolchain's LTO actually does and refuses.
- [ThinLTO: scalable and incremental LTO][thinlto] — the import-budget model
  (functions cross a boundary only under a size limit).
- [LTO, PGO and unity builds compared][jb-lto] — measured head-to-head: with LTO
  and PGO on, a unity build adds nothing significant, which is why this tree
  keeps per-file units and puts hot bodies in headers instead.
- [Link-time optimisation guide][lto-guide] — the practitioner's setup and
  pitfalls reference.
- [Structuring C projects][c-structure] — the conventional layout this tree's
  zone structure follows.

## Codegen: attributes, alignment and vectorisation

Background for the levers in
[08-idiomatic-c.md](08-idiomatic-c.md#the-c23-spellings-that-measured), which is
where this tree's own measurements live.

- [clang attribute reference][clang-attr] — what `always_inline`, `noinline` and
  `aligned` are, and are not, a request for.
- [GCC common function attributes][gcc-attr] — the same set as the
  second-compiler lane sees it.
- [clang vector extensions][clang-vec] — the `vector_size` vocabulary, and the
  alignment a vector type claims by default.
- [LLVM auto-vectorization][llvm-vec] — the loop and SLP vectorizers, their cost
  models, and the `-Rpass` remarks that report both.
- [LLVM atomics and optimisation][llvm-atomics] — which transforms an atomic
  access blocks.
- [Agner Fog's optimisation manuals][agner] — instruction tables and
  microarchitecture, per encoding: operand folding and alignment penalties.
- [Intel intrinsics guide][intel-intrinsics] — the lookup for upstream's per-ISA
  kernels.
- [What every programmer should know about memory][drepper] — cache lines,
  alignment and data layout.
- [Transparent hugepages][thp] — the alignment and size `MADV_HUGEPAGE` requires.

## Licensing

- [GNU GPL v3][gpl3] — mcfish is a derivative of Stockfish and inherits it. See
  [`../Copying.txt`](../Copying.txt) and [`../AUTHORS`](../AUTHORS).

[agner]:        https://www.agner.org/optimize/
[c-structure]:  https://www.lucavallin.com/blog/how-to-structure-c-projects-my-experience-best-practices
[clang-attr]:   https://clang.llvm.org/docs/AttributeReference.html
[clang-diag]:   https://clang.llvm.org/docs/DiagnosticsReference.html
[clang-vec]:    https://clang.llvm.org/docs/LanguageExtensions.html#vectors-and-extended-vectors
[cppref-c]:     https://en.cppreference.com/w/c
[drepper]:      https://www.akkadia.org/drepper/cpumemory.pdf
[gcc-attr]:     https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html
[intel-intrinsics]: https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html
[jb-lto]:       https://blog.jetbrains.com/clion/2022/05/testing-3-approaches-performance-cpp_apps/
[llvm-atomics]: https://llvm.org/docs/Atomics.html
[llvm-vec]:     https://llvm.org/docs/Vectorizers.html
[lto-guide]:    https://convolv.es/guides/lto/
[lto-survey]:   https://gist.github.com/MangaD/2822580b199c605009bb53c892383d93
[thinlto]:      https://storage.googleapis.com/gweb-research2023-media/pubtools/pdf/af0a39422b19fbbe063479f5d3a71d9278677314.pdf
[thp]:          https://docs.kernel.org/admin-guide/mm/transhuge.html
[cpw]:          https://www.chessprogramming.org/Main_Page
[cpw-960]:      https://www.chessprogramming.org/Chess960
[cpw-ab]:       https://www.chessprogramming.org/Alpha-Beta
[cpw-bb]:       https://www.chessprogramming.org/Bitboards
[cpw-lazysmp]:  https://www.chessprogramming.org/Lazy_SMP
[cpw-lmr]:      https://www.chessprogramming.org/Late_Move_Reductions
[cpw-magic]:    https://www.chessprogramming.org/Magic_Bitboards
[cpw-null]:     https://www.chessprogramming.org/Null_Move_Pruning
[cpw-perft]:    https://www.chessprogramming.org/Perft_Results
[cpw-qs]:       https://www.chessprogramming.org/Quiescence_Search
[cpw-tt]:       https://www.chessprogramming.org/Transposition_Table
[cpw-zobrist]:  https://www.chessprogramming.org/Zobrist_Hashing
[gpl3]:         https://www.gnu.org/licenses/gpl-3.0.html
[lc0-data]:     https://storage.lczero.org/files/training_data
[n3220]:        https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3220.pdf
[nnue-doc]:     https://github.com/official-stockfish/nnue-pytorch/blob/master/docs/nnue.md
[odbl]:         https://opendatacommons.org/licenses/odbl/odbl-10.txt
[sf-bench]:     https://github.com/official-stockfish/Stockfish/wiki/Regression-Tests
[sf-nnue]:      https://github.com/official-stockfish/Stockfish/tree/master/src/nnue
[sf-releases]:  https://github.com/official-stockfish/Stockfish/releases
[sf-syzygy]:    https://github.com/official-stockfish/Stockfish/tree/master/src/syzygy
[stockfish]:    https://github.com/official-stockfish/Stockfish
[syzygy]:       https://www.chessprogramming.org/Syzygy_Bases
[uci]:          https://backscattering.de/chess/uci/
