// Fuzz the real search in-process: no shell, no UCI protocol, no subprocess.
//
// tools/uci_fuzz.py already fuzzes the shipped binary's stdin -- a real, useful
// gate, but a subprocess driving text, so a mutation spends most of its budget
// on the parser rather than the search. This driver walks from the start
// position by playing the legal moves the fuzzer's bytes select, then hands the
// reached position straight to search_go: the same call test_main.c's
// test_search() makes, so movegen, the move picker, the TT, pruning, qsearch
// and the NNUE accumulator push/pop all run under whatever sanitizer
// ./build.sh fuzz-search compiled in, with nothing between the fuzzer and the
// node body.
//
// This links against ENGINE_SOURCES exactly as ./build.sh zone-check and
// ./build.sh test do, plus this one file -- proof by construction that the
// search needs no shell object to run, the same invariant docs/00-architecture.md
// names.
//
// Golden: none. This is test infrastructure, not a port.

#include "../src/engine/board/attacks.h"
#include "../src/engine/board/bitboard.h"
#include "../src/engine/board/legality.h"
#include "../src/engine/board/movegen.h"
#include "../src/engine/board/position.h"
#include "../src/engine/board/threats.h"
#include "../src/engine/eval/evaluate.h"
#include "../src/engine/search/search.h"
#include "../src/engine/search/tt.h"

#include <stddef.h>
#include <stdint.h>

// Random-walk depth cap. Deep enough to reach positions no bench list or golden
// covers -- the same reason tools/upstream_nodes.py walks from the start
// position rather than a fixed FEN table -- shallow enough that a short input
// still reaches a search rather than spending its whole budget walking.
enum { FUZZ_MAX_WALK_PLIES = 40 };

// Search depth cap. Fixed and shallow ON PURPOSE: libFuzzer wants thousands of
// iterations per second, and a bug in the node body is found as reliably at
// depth 3 as at depth 8 -- the cost of a deeper search buys coverage of ITSELF
// (more nodes at the same bug), not of a different code path.
enum { FUZZ_SEARCH_DEPTH = 3 };

// The libFuzzer runtime calls these two by name; no mcfish header declares them
// because no mcfish translation unit calls them. Declare them here, once, so
// -Wmissing-prototypes has the header-in-the-same-file every other symbol in
// this tree gets (docs/08-idiomatic-c.md's warning table).
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void) argc;
    (void) argv;

    // Same order main.c uses, and for the same reason: a Position built before
    // attacks_init reads zeroed attack tables. See 00-architecture.md.
    bitboards_init();
    attacks_init();
    threats_init();
    position_init();

    // Best-effort. A missing net leaves the classical fallback in place rather
    // than failing (eval_nnue_load's own contract), so a fuzz run with no
    // resources/ still exercises movegen, the picker, the TT and pruning --
    // just not the NNUE accumulator path. "resources/" matches the literal
    // tests/test_main.c uses, both relative to the repo root build.sh invokes
    // from.
    (void) eval_nnue_init();
    (void) eval_nnue_load("resources/", nullptr);

    // 1 MB is plenty for a depth-3 search and keeps each process's RSS small
    // under many parallel libFuzzer workers.
    (void) tt_resize(1);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Position pos;
    StateInfo root_st;
    StateInfo walk_st[FUZZ_MAX_WALK_PLIES];

    if (!pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false,
                 &root_st))
        return 0;

    size_t cursor = 0;
    for (int ply = 0; ply < FUZZ_MAX_WALK_PLIES && cursor < size; ply++) {
        ExtMove list[MAX_MOVES];
        const ExtMove *const end = generate_legal(&pos, list);
        const ptrdiff_t count = end - list;
        if (count <= 0)
            break;  // checkmate or stalemate: nowhere left to walk

        const Move m = list[data[cursor++] % (size_t) count].move;
        const bool gives_check = pos_gives_check(&pos, m);
        pos_do_move(&pos, m, &walk_st[ply], gives_check, &pos.scratch_dp, &pos.scratch_dts,
                    nullptr);
    }

    const SearchLimits limits = { .depth = FUZZ_SEARCH_DEPTH };
    (void) search_go(&pos, &limits);
    return 0;
}
