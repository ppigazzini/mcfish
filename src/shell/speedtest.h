// Own the `speedtest` command: upstream's long-form throughput measurement.
//
// It is not `bench`. `bench` fixes a DEPTH over 51 positions and its node total is
// this repo's bit-exactness anchor; `speedtest` fixes a TIME budget over five real
// games and reports nodes per second, which is a property of the machine and the
// build and of nothing that can be pinned. The two are complementary and neither
// substitutes for the other: a change that costs 3% of throughput moves nothing
// `bench` prints.
//
// Golden: upstream `uci.cpp:315` (UCIEngine::benchmark) and `benchmark.cpp:466`
// (setup_benchmark).

#ifndef MCFISH_SPEEDTEST_H
#define MCFISH_SPEEDTEST_H

// Run the speedtest. ARGS is the rest of the command line: `[threads] [ttSize]
// [seconds]`, each defaulting when the line runs dry -- the hardware concurrency,
// 128 MiB per thread, and 150 seconds. Every line it prints goes to stderr, as
// upstream's does, so a `2>/dev/null` run prints nothing at all.
void speedtest_run(const char *args);

#endif  // MCFISH_SPEEDTEST_H
