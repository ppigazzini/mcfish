# References

Links only. Anything a reader could learn from one of these does not belong in
the rest of this set — see [11-writing.md](11-writing.md).

Audience: all developers.

## The repository that defines the work

mcfish is a C23 port of **Stockfish**, which is the reference implementation for
everything here. See [PORTING.md](PORTING.md) for where the code comes from.

- [Stockfish][stockfish] — the **golden**. It defines correct behaviour, and the
  differential gate compares against a pristine upstream build. Where mcfish and
  Stockfish disagree, Stockfish wins. The module-by-module mapping is
  `tools/upstream/port_map.tsv`.
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
  `src/platform/` and is not in the build; see [06-platform.md](06-platform.md).
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

## Licensing

- [GNU GPL v3][gpl3] — mcfish is a derivative of Stockfish and inherits it. See
  [`../Copying.txt`](../Copying.txt) and [`../AUTHORS`](../AUTHORS).

[clang-diag]:   https://clang.llvm.org/docs/DiagnosticsReference.html
[cppref-c]:     https://en.cppreference.com/w/c
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
