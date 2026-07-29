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

    // eval_nnue_init() is NOT called here. It allocates the eval arena, and the
    // host's arena source (the huge-page-backed page_alloc/page_free pair) is not
    // registered until engine_init runs, inside uci_loop -- calling it any earlier
    // would hand the arena to the plain-malloc fallback and then free it through
    // page_free once engine_init rewires ArenaFree, corrupting the free. engine_init
    // calls it itself, right after the arena source, for the same reason
    // search_clear is sequenced there (see the comment on that line).
    uci_loop(argc, argv);
    search_shutdown();
    eval_nnue_shutdown();
    return 0;
}
