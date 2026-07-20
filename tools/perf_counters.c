// Run interleaved paired A/B over CPU HARDWARE COUNTERS, on EVERY arch tier.
//
// WHY THIS EXISTS. perf_callgrind.sh gives deterministic instruction counts, but ONLY on
// sse41: callgrind (valgrind) SIGILLs on the avx512 EVEX prefix (0x62), so the top tiers --
// avx2, native/vnni512 -- were never measured directly. The `perf` *binary* is absent under
// WSL2, but `perf_event_open` is not, and it is the syscall that matters. Call it directly
// and every tier is measurable, including vnni512, at native speed.
//
// WHAT IT ADDS. nps_ab.sh gives wall-clock only, which is thermally noisy. callgrind gives
// deterministic instructions but sse41-only and at ~50x slowdown, and cannot see cycles/IPC.
// This gives BOTH: instructions (the work) AND cycles/IPC/cache-misses (the efficiency), at
// native speed, on every tier -- the only tool here that can SEE an IPC/memory gap rather
// than infer one.
//
// THE PROTOCOL IS THE POINT (every rule was paid for by a wrong result upstream in zfish):
//   * INTERLEAVE A and B, alternating in one loop -- never two readings from different moments.
//   * TAKE THE MEDIAN OF PER-ROUND PAIRED RATIOS, not the ratio of medians -- the two disagreed
//     by 2x on a real change.
//   * PIN to one core, so both binaries see the same thermal/frequency state.
//   * ASSERT NODE COUNTS EQUAL: a different tree is a different workload and every ratio would
//     be meaningless. Refuse to report if they differ.
//
// Instructions are near-deterministic and are the trustworthy headline; cycles/IPC carry
// thermal noise, which is why they are reported as interleaved paired ratios.
//
// GATING. Set MAX_INSTR_RATIO to make this a regression gate: it exits non-zero when the
// median paired INSTRUCTION ratio exceeds that bound. Gate on the RATIO against the oracle,
// not an absolute count: the ratio cancels machine, libc and net-load differences. Keep it a
// LOCAL gate -- perf_event_open can be refused inside CI containers -- run it before committing
// perf work. Port of zfish tools/perf_counters.zig.
//
// Usage (CWD must be resources/ so the net loads):
//   perf_counters <binA> <binB> <rounds> [bench-args...]
//   e.g: perf_counters ../build/mcfish $ORACLE/src/stockfish 8 bench 16 1 13
//   MAX_INSTR_RATIO=1.36 perf_counters ../build/mcfish $ORACLE/src/stockfish 8 bench 16 1 13

#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    uint64_t instructions;
    uint64_t cycles;
    uint64_t cache_misses;
    uint64_t branch_misses;
    uint64_t nodes;
} Counters;

static double ipc_of(Counters c) {
    return c.cycles == 0 ? 0.0 : (double) c.instructions / (double) c.cycles;
}

static int open_counter(uint64_t config, pid_t pid) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof attr;
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.inherit = 1;  // count the child's threads too (Lazy-SMP workers)
    long fd = syscall(SYS_perf_event_open, &attr, pid, -1, -1, 0UL);
    return (int) fd;
}

// Parse "Nodes searched  : N" out of the child's bench output. Without this the tool would
// happily compare two different trees.
static uint64_t parse_nodes(const char *text, size_t len) {
    const char marker[] = "Nodes searched";
    const size_t mlen = sizeof marker - 1;
    for (size_t i = 0; i + mlen <= len; i++) {
        if (memcmp(text + i, marker, mlen) != 0)
            continue;
        size_t j = i + mlen;
        while (j < len && (text[j] == ' ' || text[j] == ':'))
            j++;
        uint64_t n = 0;
        size_t start = j;
        while (j < len && text[j] >= '0' && text[j] <= '9')
            n = n * 10 + (uint64_t) (text[j++] - '0');
        if (j > start)
            return n;
    }
    return 0;
}

static Counters run_once(char *const argv[], int core) {
    Counters result;
    memset(&result, 0, sizeof result);

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        perror("pipe");
        exit(2);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child: pin to one core so A and B see identical thermal/frequency state.
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        sched_setaffinity(0, sizeof set, &set);

        close(pipe_fds[0]);
        // The engines print the bench summary (and the node count this tool gates on) to
        // stderr, not stdout -- capture BOTH.
        dup2(pipe_fds[1], 1);
        dup2(pipe_fds[1], 2);
        close(pipe_fds[1]);

        // Stop before exec so the parent can arm the counters BEFORE any work runs.
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        kill(getpid(), SIGSTOP);

        execv(argv[0], argv);
        _exit(127);
    }
    close(pipe_fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);  // wait for the child's SIGSTOP

    int c_instr = open_counter(PERF_COUNT_HW_INSTRUCTIONS, pid);
    int c_cyc = open_counter(PERF_COUNT_HW_CPU_CYCLES, pid);
    int c_cache = open_counter(PERF_COUNT_HW_CACHE_MISSES, pid);
    int c_branch = open_counter(PERF_COUNT_HW_BRANCH_MISSES, pid);
    if (c_instr < 0 || c_cyc < 0) {
        fprintf(stderr,
                "error: perf_event_open failed (need perf_event_paranoid <= 1 or CAP_PERFMON;\n"
                "       it can also be refused inside a container). errno path.\n");
        exit(2);
    }

    int fds[4] = { c_instr, c_cyc, c_cache, c_branch };
    for (int i = 0; i < 4; i++)
        if (fds[i] >= 0) {
            ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
            ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0);
        }
    ptrace(PTRACE_DETACH, pid, 0, 0);

    // Drain the pipe while the child runs, or a full pipe deadlocks it.
    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipe_fds[0], buf, sizeof buf);
        if (n <= 0)
            break;
        if (out_len + (size_t) n + 1 > out_cap) {
            out_cap = (out_cap ? out_cap * 2 : 8192) + (size_t) n;
            out = realloc(out, out_cap);
        }
        memcpy(out + out_len, buf, (size_t) n);
        out_len += (size_t) n;
    }
    close(pipe_fds[0]);
    waitpid(pid, &status, 0);

    for (int i = 0; i < 4; i++)
        if (fds[i] >= 0)
            ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);

    if (read(c_instr, &result.instructions, 8) != 8)
        result.instructions = 0;
    if (read(c_cyc, &result.cycles, 8) != 8)
        result.cycles = 0;
    if (c_cache >= 0 && read(c_cache, &result.cache_misses, 8) != 8)
        result.cache_misses = 0;
    if (c_branch >= 0 && read(c_branch, &result.branch_misses, 8) != 8)
        result.branch_misses = 0;

    for (int i = 0; i < 4; i++)
        if (fds[i] >= 0)
            close(fds[i]);

    result.nodes = out ? parse_nodes(out, out_len) : 0;
    free(out);
    return result;
}

static double ratio(uint64_t a, uint64_t b) {
    return b == 0 ? 0.0 : (double) a / (double) b;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static double median(double *values, size_t n) {
    qsort(values, n, sizeof values[0], cmp_double);
    if (n == 0)
        return 0;
    return (n % 2 == 1) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: perf_counters <binA> <binB> <rounds> [bench-args...]  (CWD = resources/)\n"
                "   e.g: perf_counters ../build/mcfish $ORACLE/src/stockfish 8 bench 16 1 13\n\n"
                "Interleaved paired A/B over hardware counters. Reports instructions (the work)\n"
                "and cycles/IPC/cache-misses (the efficiency). Works on EVERY arch, incl.\n"
                "avx512/vnni512 where callgrind SIGILLs. Refuses to report if node counts differ.\n");
        return 2;
    }

    const char *bin_a = argv[1];
    const char *bin_b = argv[2];
    size_t rounds = strtoul(argv[3], NULL, 10);
    if (rounds == 0)
        rounds = 8;

    // Build the two argv vectors: <bin> [bench-args...] NULL.
    size_t extra = (size_t) argc - 4;
    char **argv_a = calloc(extra + 2, sizeof *argv_a);
    char **argv_b = calloc(extra + 2, sizeof *argv_b);
    argv_a[0] = (char *) bin_a;
    argv_b[0] = (char *) bin_b;
    for (size_t i = 0; i < extra; i++) {
        argv_a[i + 1] = argv[4 + i];
        argv_b[i + 1] = argv[4 + i];
    }

    double *r_instr = calloc(rounds, sizeof *r_instr);
    double *r_cyc = calloc(rounds, sizeof *r_cyc);
    double *r_ipc = calloc(rounds, sizeof *r_ipc);
    double *r_cache = calloc(rounds, sizeof *r_cache);

    for (size_t i = 0; i < rounds; i++) {
        Counters a = run_once(argv_a, 0);
        Counters b = run_once(argv_b, 0);
        if (i == 0) {
            if (a.nodes == 0 || b.nodes == 0) {
                fprintf(stderr,
                        "error: could not parse a node count (A=%lu, B=%lu).\n"
                        "       Run from resources/ so the net loads, and use a `bench` command.\n",
                        (unsigned long) a.nodes, (unsigned long) b.nodes);
                return 2;
            }
            if (a.nodes != b.nodes) {
                fprintf(stderr,
                        "error: node counts differ (A=%lu, B=%lu).\n"
                        "       Different trees = different workloads; every ratio meaningless.\n",
                        (unsigned long) a.nodes, (unsigned long) b.nodes);
                return 2;
            }
            printf("# tree: %lu nodes (identical on both) | %zu rounds | core 0\n",
                   (unsigned long) a.nodes, rounds);
            printf("# %5s %16s %16s %9s %8s %8s\n", "round", "A instr", "B instr", "A/B instr",
                   "A IPC", "B IPC");
        }
        r_instr[i] = ratio(a.instructions, b.instructions);
        r_cyc[i] = ratio(a.cycles, b.cycles);
        r_ipc[i] = ipc_of(b) > 0 ? ipc_of(a) / ipc_of(b) : 0;
        r_cache[i] = ratio(a.cache_misses, b.cache_misses);
        printf("  %5zu %16lu %16lu %9.3f %8.3f %8.3f\n", i + 1,
               (unsigned long) a.instructions, (unsigned long) b.instructions, r_instr[i],
               ipc_of(a), ipc_of(b));
        fflush(stdout);
    }

    printf("\n# MEDIAN PAIRED A/B RATIOS (A is the first binary)\n");
    printf("#   instructions : %.3f   <- the WORK. near-deterministic; trust this most.\n",
           median(r_instr, rounds));
    printf("#   cycles       : %.3f   <- the TIME. carries thermal noise.\n",
           median(r_cyc, rounds));
    printf("#   IPC          : %.3f   <- the EFFICIENCY. <1 means A retires fewer instr/cycle.\n",
           median(r_ipc, rounds));
    printf("#   cache misses : %.3f\n", median(r_cache, rounds));
    printf("#\n"
           "# READ IT THIS WAY: cycles ~= instructions / IPC. If A's cycle ratio is worse than\n"
           "# its instruction ratio, the residue is an IPC/memory gap -- A does similar work but\n"
           "# retires it slower -- and no instruction-count cut closes that half.\n");

    // Regression gate when MAX_INSTR_RATIO is set.
    const char *bound_str = getenv("MAX_INSTR_RATIO");
    if (!bound_str)
        return 0;
    char *end = NULL;
    double bound = strtod(bound_str, &end);
    if (end == bound_str) {
        fprintf(stderr, "error: MAX_INSTR_RATIO=%s is not a number\n", bound_str);
        return 2;
    }
    double got = median(r_instr, rounds);
    if (got > bound) {
        printf("\nFAIL: instruction ratio %.4f exceeds MAX_INSTR_RATIO %.4f\n", got, bound);
        return 1;
    }
    printf("\nOK: instruction ratio %.4f within MAX_INSTR_RATIO %.4f\n", got, bound);
    return 0;
}
