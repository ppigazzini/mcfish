// Statistical CYCLE profiler over perf_event_open sampling -- a minimal `perf record` that
// needs no `perf` binary and runs at native speed on EVERY arch tier (unlike callgrind, which
// only counts INSTRUCTIONS and SIGILLs on avx512). callgrind cannot see where CYCLES go; this
// can, so it is the tool for an IPC gap -- attributing wall-clock cost to the symbol that
// actually burns it, not the one that merely retires the most instructions.
//
// Attaches a CPU_CYCLES sampling event to EVERY thread of the child (the engine runs its
// search on a worker thread even at Threads 1, so a main-thread-only event sees startup and
// idle, never the search), mmaps each ring, drains PERF_RECORD_SAMPLE IPs, and folds each IP
// to the nearest `nm -n` symbol of the engine binary (PIE base-relative) or to its mapped
// object otherwise.
//
// Usage (CWD must be resources/ so the net loads):
//   perf_sample <binary> <period_cycles> <bench-args...>
//   perf_sample ../build/mcfish 40000 bench 500 1 16

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// --- symbol table of the engine binary (from `nm -n`) ---------------------------------
typedef struct {
    uint64_t addr;
    char name[96];
} Sym;
static Sym *syms;
static size_t nsym;

static void load_symbols(const char *binary) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "nm -n --defined-only '%s' 2>/dev/null | grep -iE ' [tTwW] '", binary);
    FILE *p = popen(cmd, "r");
    if (!p) {
        fprintf(stderr, "error: nm failed on %s\n", binary);
        exit(2);
    }
    size_t cap = 4096;
    syms = malloc(cap * sizeof *syms);
    char line[512];
    while (fgets(line, sizeof line, p)) {
        unsigned long a;
        char t, nm[256];
        if (sscanf(line, "%lx %c %255s", &a, &t, nm) == 3) {
            if (nsym == cap) {
                cap *= 2;
                syms = realloc(syms, cap * sizeof *syms);
            }
            syms[nsym].addr = a;
            snprintf(syms[nsym].name, sizeof syms[nsym].name, "%s", nm);
            nsym++;
        }
    }
    pclose(p);
}

const char *g_focus;  // PS_FOCUS symbol name, or NULL

static size_t sym_index(uint64_t off) {
    if (nsym == 0 || off < syms[0].addr)
        return (size_t) -1;
    size_t lo = 0, hi = nsym - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (syms[mid].addr <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static const char *sym_for(uint64_t off) {
    size_t i = sym_index(off);
    return i == (size_t) -1 ? "?" : syms[i].name;
}

static uint64_t sym_start_for(uint64_t off) {
    size_t i = sym_index(off);
    return i == (size_t) -1 ? 0 : syms[i].addr;
}

// --- runtime maps of the child --------------------------------------------------------
typedef struct {
    uint64_t start, end;
    char obj[256];
} MapRange;
static MapRange *ranges;
static size_t nrange;
static uint64_t bin_base = 0;
static const char *bin_path;

static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void read_maps(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t cap = 256;
    free(ranges);
    ranges = malloc(cap * sizeof *ranges);
    nrange = 0;
    const char *bin_bn = basename_of(bin_path);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        uint64_t s, e;
        char perms[8], obj[256] = "";
        int n = sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*u %255s", &s, &e, perms, obj);
        if (n < 3)
            continue;
        if (nrange == cap) {
            cap *= 2;
            ranges = realloc(ranges, cap * sizeof *ranges);
        }
        ranges[nrange].start = s;
        ranges[nrange].end = e;
        snprintf(ranges[nrange].obj, sizeof ranges[nrange].obj, "%s",
                 n >= 4 ? basename_of(obj) : "[anon]");
        // PIE bias = the LOWEST mapping of the engine binary (its file-offset-0 segment).
        if (n >= 4 && strcmp(basename_of(obj), bin_bn) == 0 && (bin_base == 0 || s < bin_base))
            bin_base = s;
        nrange++;
    }
    fclose(f);
}

static const char *obj_for_ip(uint64_t ip) {
    for (size_t i = 0; i < nrange; i++)
        if (ip >= ranges[i].start && ip < ranges[i].end)
            return ranges[i].obj;
    return "[unknown]";
}

// --- sample buckets -------------------------------------------------------------------
typedef struct {
    char key[128];
    uint64_t count;
} Bucket;
static Bucket *buckets;
static size_t nbucket, bcap;

static void bump(const char *key) {
    for (size_t i = 0; i < nbucket; i++)
        if (strcmp(buckets[i].key, key) == 0) {
            buckets[i].count++;
            return;
        }
    if (nbucket == bcap) {
        bcap = bcap ? bcap * 2 : 512;
        buckets = realloc(buckets, bcap * sizeof *buckets);
    }
    snprintf(buckets[nbucket].key, sizeof buckets[nbucket].key, "%s", key);
    buckets[nbucket].count = 1;
    nbucket++;
}

static int cmp_bucket(const void *a, const void *b) {
    uint64_t x = ((const Bucket *) a)->count, y = ((const Bucket *) b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

// --- per-thread sampling buffers ------------------------------------------------------
#define MMAP_PAGES 512
typedef struct {
    int tid, fd, maps_read;
    struct perf_event_mmap_page *meta;
    uint8_t *data;
} TBuf;
static TBuf tbufs[128];
static size_t ntbuf;
static uint64_t g_total, g_resolved;
static uint64_t g_period;
static size_t g_page, g_data_size;

static int seen_tid(int tid) {
    for (size_t i = 0; i < ntbuf; i++)
        if (tbufs[i].tid == tid)
            return 1;
    return 0;
}

static void attach_tid(int tid) {
    if (ntbuf >= 128 || seen_tid(tid))
        return;
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof attr;
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    attr.sample_period = g_period;
    attr.sample_type = PERF_SAMPLE_IP;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    int fd = (int) syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0UL);
    if (fd < 0)
        return;
    void *base = mmap(NULL, (MMAP_PAGES + 1) * g_page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return;
    }
    tbufs[ntbuf].tid = tid;
    tbufs[ntbuf].fd = fd;
    tbufs[ntbuf].meta = base;
    tbufs[ntbuf].data = (uint8_t *) base + g_page;
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    ntbuf++;
}

static void poll_tasks(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
            attach_tid(atoi(e->d_name));
    closedir(d);
}

static void drain(TBuf *tb) {
    uint64_t head = tb->meta->data_head;
    __sync_synchronize();
    uint64_t tail = tb->meta->data_tail;
    while (tail < head) {
        struct perf_event_header hh;
        for (size_t i = 0; i < sizeof hh; i++)
            ((uint8_t *) &hh)[i] = tb->data[(tail + i) % g_data_size];
        if (hh.size == 0)
            break;
        if (hh.type == PERF_RECORD_SAMPLE) {
            uint64_t ip;
            for (size_t i = 0; i < 8; i++)
                ((uint8_t *) &ip)[i] = tb->data[(tail + sizeof hh + i) % g_data_size];
            g_total++;
            if (bin_base && ip >= bin_base && strcmp(obj_for_ip(ip), basename_of(bin_path)) == 0) {
                const char *s = sym_for(ip - bin_base);
                // PS_FOCUS=<symbol>: bucket by instruction offset within that symbol, so the
                // hot vector-op ranges inside a big inlined kernel become visible (map with
                // `objdump -d`). Falls through to per-symbol bucketing otherwise.
                if (g_focus && strcmp(s, g_focus) == 0) {
                    uint64_t off = (ip - bin_base) - sym_start_for(ip - bin_base);
                    char k[128];
                    snprintf(k, sizeof k, "%s+0x%lx", s, (unsigned long) (off & ~0xfUL));
                    bump(k);
                } else {
                    bump(s);
                }
                g_resolved++;
            } else {
                char k[128];
                snprintf(k, sizeof k, "<%s>", obj_for_ip(ip));
                bump(k);
            }
        }
        tail += hh.size;
    }
    __sync_synchronize();
    tb->meta->data_tail = head;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: perf_sample <binary> <period_cycles> [bench-args...]  (CWD=resources/)\n"
                        "   or: PS_UCI='uci\\nposition...\\ngo movetime N' perf_sample <binary> <period>\n");
        return 2;
    }
    bin_path = argv[1];
    g_period = strtoull(argv[2], NULL, 10);
    g_focus = getenv("PS_FOCUS");
    // Child argv = binary + bench-args (argv[3..]); the period (argv[2]) is NOT passed on.
    char *child_argv[64];
    size_t ca = 0;
    child_argv[ca++] = argv[1];
    for (int i = 3; i < argc && ca < 62; i++)
        child_argv[ca++] = argv[i];
    child_argv[ca] = NULL;
    g_page = (size_t) sysconf(_SC_PAGESIZE);
    g_data_size = (size_t) MMAP_PAGES * g_page;
    load_symbols(bin_path);

    // PS_UCI: newline-separated UCI commands (with literal "\n") fed on the child's stdin, for
    // a single long search that keeps ONE worker thread alive the whole run. `bench` instead
    // spawns a short-lived worker PER position, which per-thread sampling mostly misses.
    const char *ps_uci = getenv("PS_UCI");

    int pipe_fds[2], in_fds[2];
    if (pipe(pipe_fds) != 0 || pipe(in_fds) != 0) {
        perror("pipe");
        return 2;
    }
    pid_t pid = fork();
    if (pid == 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        sched_setaffinity(0, sizeof set, &set);
        close(pipe_fds[0]);
        dup2(pipe_fds[1], 1);
        dup2(pipe_fds[1], 2);
        close(pipe_fds[1]);
        if (ps_uci) {
            close(in_fds[1]);
            dup2(in_fds[0], 0);
            close(in_fds[0]);
        }
        execv(child_argv[0], child_argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    close(in_fds[0]);
    if (ps_uci) {
        // Expand literal "\n" into newlines and feed all commands. Keep stdin OPEN: an early
        // EOF makes some engines (Stockfish) treat it as `quit` and abort the search mid-run.
        for (const char *c = ps_uci; *c; c++) {
            if (c[0] == '\\' && c[1] == 'n') {
                (void) !write(in_fds[1], "\n", 1);
                c++;
            } else {
                (void) !write(in_fds[1], c, 1);
            }
        }
        (void) !write(in_fds[1], "\n", 1);
    } else {
        close(in_fds[1]);
    }
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    int quit_sent = 0;

    char buf[65536];
    int status = 0;
    for (;;) {
        read_maps(pid);       // refresh maps (cheap) so late mmaps and the PIE base resolve
        poll_tasks(pid);      // attach to any thread not yet sampled (the search worker)
        for (size_t i = 0; i < ntbuf; i++)
            drain(&tbufs[i]);
        ssize_t r;
        while ((r = read(pipe_fds[0], buf, sizeof buf)) > 0) {
            // On `bestmove`, the search is done: quit cleanly (keeps stdin-open engines from
            // waiting forever) after the samples have been drained.
            if (ps_uci && !quit_sent && memmem(buf, (size_t) r, "bestmove", 8)) {
                (void) !write(in_fds[1], "quit\n", 5);
                close(in_fds[1]);
                quit_sent = 1;
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            for (size_t i = 0; i < ntbuf; i++)
                drain(&tbufs[i]);
            break;
        }
        const struct timespec ts = { 0, 150000 };
        nanosleep(&ts, NULL);
    }

    qsort(buckets, nbucket, sizeof *buckets, cmp_bucket);
    printf("# cycle samples: %lu total, %lu in engine binary (%.1f%%), period=%lu, threads=%zu\n",
           (unsigned long) g_total, (unsigned long) g_resolved,
           g_total ? 100.0 * (double) g_resolved / (double) g_total : 0.0,
           (unsigned long) g_period, ntbuf);
    printf("# %-44s %9s %7s\n", "symbol", "samples", "cycles%");
    for (size_t i = 0; i < nbucket && i < 30; i++)
        printf("  %-44s %9lu %6.2f%%\n", buckets[i].key, (unsigned long) buckets[i].count,
               g_total ? 100.0 * (double) buckets[i].count / (double) g_total : 0.0);
    return 0;
}
