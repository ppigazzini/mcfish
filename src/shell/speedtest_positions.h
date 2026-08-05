// Hold `speedtest`'s position list: five games, one side's moves only, verbatim from
// upstream.
//
// This is pure data and nothing else -- no search, no stdio, no dependency, exactly
// as bench_positions.h is for `bench`. The invariant is the same and matters for the
// same reason: the table IS the workload, so an entry added, removed or reordered
// makes every number this command has ever printed incomparable, both against
// upstream and against this tree's own past runs. Unlike `bench` it pins no anchor --
// a speedtest is a TIMING, not a node count -- which is precisely why nothing would
// go red if it drifted.
//
// Each game is a NULL-terminated array of FEN records, played in order; the ply index
// within a game is what sets that position's movetime (see speedtest.c).
//
// Golden: upstream `benchmark.cpp:105` (BenchmarkPositions), "human-randomly picked 5
// games with <60 moves" from tests.stockfishchess.org/tests/view/665c71f9fd45fb0f907c21e0.

#ifndef MCFISH_SPEEDTEST_POSITIONS_H
#define MCFISH_SPEEDTEST_POSITIONS_H

// Read the game list. Static storage, immutable, valid for the process.
extern const char *const *const SpeedtestGames[];
extern const int SpeedtestGameCount;

#endif  // MCFISH_SPEEDTEST_POSITIONS_H
