#include "benchmark.h"

#include "../engine/board/position.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/search.h"
#include "../engine/search/tt.h"
#include "../platform/clock.h"

#include <stdio.h>

// Hold the bench set. These are upstream Stockfish's bench positions, kept
// verbatim so a position-by-position node comparison against upstream stays
// meaningful even though the totals differ: the search is still ccfish's, not
// the ported node bodies.
const char *const BenchFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "rq3rk1/ppp2ppp/1bnpb3/3N2B1/3NP3/7P/PPPQ1PP1/2KR3R w - - 7 14",
    "r1bq1r1k/1pp1n1pp/1p1p4/4p2Q/4Pp2/1BNP4/PPP2PPP/3R1RK1 w - - 2 14",
    "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
    "r1bbk1nr/pp3p1p/2n5/1N4p1/2Np1B2/8/PPP2PPP/2KR1B1R w kq - 0 13",
    "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
    "4r1k1/r1q2ppp/ppp2n2/4P3/5Rb1/1N1BQ3/PPP3PP/R5K1 w - - 1 17",
    "2rqkb1r/ppp2p2/2npb1p1/1N1Nn2p/2P1PP2/8/PP2B1PP/R1BQK2R b KQ - 0 11",
    "r1bq1r1k/b1p1npp1/p2p3p/1p6/3PP3/1B2NN2/PP3PPP/R2Q1RK1 w - - 1 16",
    "3r1rk1/p4pp1/2p1p2p/qpQP3P/2P5/1P4P1/P3BP2/2R1R1K1 b - - 0 20",
    "r1bqk2r/pp2bppp/2p5/3pP3/P2Q1P2/2N1B3/1PP3PP/R4RK1 b kq - 0 13",
    "8/8/8/8/5kp1/P7/8/1K1N4 w - - 0 1",
    "8/2p4P/8/kr6/6R1/8/8/1K6 w - - 0 1",
};

const int BenchFenCount = (int) (sizeof BenchFens / sizeof BenchFens[0]);

uint64_t benchmark_run(int depth) {
    uint64_t total = 0;
    const uint64_t start = now_ms();

    // Name the evaluation the run used, on the banner stream the totals go to. The
    // signature anchor is defined with a net loaded, and a fallback run produces an
    // unrelated number, so the two must never be told apart by eye alone.
    fprintf(stderr, "info string %s\n", eval_nnue_status());

    // Clear the table between positions: a signature that depended on carried-over
    // entries would change with the position order and with the Hash setting.
    for (int i = 0; i < BenchFenCount; ++i) {
        Position pos;
        StateInfo st;

        if (!pos_set(&pos, BenchFens[i], false, &st)) {
            fprintf(stderr, "bench: malformed FEN at index %d\n", i);
            continue;
        }

        tt_clear();

        SearchLimits limits = { 0 };
        limits.depth = depth;

        fprintf(stderr, "\nPosition: %d/%d (%s)\n", i + 1, BenchFenCount, BenchFens[i]);
        const SearchResult r = search_go(&pos, &limits);
        total += r.nodes;
    }

    const uint64_t elapsed = now_ms() - start;
    const uint64_t ms = elapsed > 0 ? elapsed : 1;

    fprintf(stderr, "\n===========================\n");
    fprintf(stderr, "Total time (ms) : %llu\n", (unsigned long long) elapsed);
    fprintf(stderr, "Nodes searched  : %llu\n", (unsigned long long) total);
    fprintf(stderr, "Nodes/second    : %llu\n", (unsigned long long) (total * 1000 / ms));

    return total;
}
