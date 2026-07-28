// Validate what a CPU performance counter actually counts, before believing it.
//
// WHY THIS EXISTS. A counter opened by name is a hypothesis, not a measurement. This
// repository has now twice drawn a wrong conclusion from an event whose documented
// name did not describe its behaviour on this host, and the second time the error
// survived long enough to be reported as a finding. The cost of checking is one
// minute; the cost of not checking is a campaign aimed at the wrong end of the
// machine.
//
// THE METHOD. Run two loops whose bottleneck is known from first principles and
// check that the counter moves the way the bottleneck demands. If it does not, the
// event does not mean what its name says -- on this host, with this kernel, at this
// microcode level -- and nothing may be built on it.
//
//   chain  ONE serial dependency chain of 3-cycle multiplies. The op is always ready
//          to dispatch and the machine still cannot retire faster than the latency,
//          so this is LATENCY-bound and its IPC pins near 1.
//   ilp    Four INDEPENDENT chains. Nothing waits on anything, so this is
//          THROUGHPUT-bound and its IPC runs to 3+.
//
// Both carry an empty asm barrier on the accumulators. Without it the optimizer
// folds either loop into a closed form: the first attempt at this measured 0.5
// instructions per iteration, which is to say it measured nothing at all.
//
// It prints a `Nodes searched:` line so perf_counters accepts it as a subject and
// its node-parity assertion passes trivially.
//
// WORKED EXAMPLE, and the reason perf_counters does NOT offer these events. AMD
// PMCx1A0 de_no_dispatch_per_slot, umask 0x01 and 0x02, whose documented names are
// "no ops from front end" and "back-end stalls":
//
//   chain  IPC 1.00   fe    8M   be     1M
//   ilp    IPC 3.30   fe   57M   be  1775M
//
// The latency-bound loop -- the textbook back-end stall -- reads essentially ZERO on
// the back-end column, and the FAST loop reads 1775M. The events count DISPATCH
// pressure: in the chain each op dispatches at once and then waits in the scheduler,
// which is not a dispatch stall, while in the ILP loop the front end runs ahead and
// fills the machine, which is. A higher back-end number therefore means the front
// end is running further ahead, and that accompanies fast code as readily as slow.
// No cycle deficit can be read out of them. They were briefly wired into
// perf_counters and produced two wrong findings before this check was run, so they
// were removed rather than documented -- a tool should not offer a foot-gun whose
// only track record is self-harm. This file is where the lesson lives instead: if a
// counter is worth adding, run it through here first.
//
// Build:  clang -O2 -std=c23 -o /tmp/pcv tools/perf_counter_validate.c
// Run:    cd resources && ../tools/perf_counters.sh /tmp/pcv /tmp/pcv 2 chain
//         cd resources && ../tools/perf_counters.sh /tmp/pcv /tmp/pcv 2 ilp
// with whatever counter is under test wired in, and check the response against the
// bottleneck each loop is known to have.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const uint64_t n = 300000000;
    uint64_t a = 1, b = 2, c = 3, d = 4;

    if (argc > 1 && strcmp(argv[1], "ilp") == 0) {
        for (uint64_t i = 0; i < n; i++) {
            a = a * 3 + 1;
            b = b * 3 + 1;
            c = c * 3 + 1;
            d = d * 3 + 1;
            __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
        }
    } else {
        for (uint64_t i = 0; i < n; i++) {
            a = a * 6364136223846793005ULL + 1442695040888963407ULL;
            __asm__ volatile("" : "+r"(a));
        }
    }

    // perf_counters gates on this line and refuses to report without it.
    printf("Nodes searched: 1\n%llu\n", (unsigned long long) (a + b + c + d));
    return 0;
}
