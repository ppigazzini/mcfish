// Compose the process: initialise the static tables in dependency order, then
// hand control to the UCI loop.
//
// The order is load-bearing and each step feeds the next: bitboards_init writes
// SquareBB, attacks_init runs the magic search and derives the square-pair
// geometry from it, and every Position built afterwards reads those tables. A
// position created before attacks_init sees zeroed attack sets and generates no
// moves — a failure that looks like a search bug, not a startup one.

#include "../engine/board/attacks.h"
#include "../engine/board/bitboard.h"
#include "../engine/board/position.h"
#include "../engine/board/threats.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "uci.h"

int main(int argc, char **argv) {
    bitboards_init();
    attacks_init();
    threats_init();  // build RayPassBB, which reads the attack tables
    position_init();

    // Build the NNUE feature tables and arenas in the same phase: they are zero,
    // not garbage, beforehand, so a missing call is a silent all-zero feature set
    // rather than a crash. The net itself
    // is loaded by the UCI layer, which owns the EvalFile option.
    eval_nnue_init();

    uci_loop(argc, argv);
    search_shutdown();
    eval_nnue_shutdown();
    return 0;
}
