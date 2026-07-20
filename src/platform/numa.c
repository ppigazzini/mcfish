// Define _GNU_SOURCE before any libc header: cpu_set_t, CPU_SET/CPU_ISSET and
// sched_getaffinity sit behind glibc's __USE_GNU guard, which -D_POSIX_C_SOURCE=200809L
// alone does not open.
#define _GNU_SOURCE

#include "numa.h"

#include "thread.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const size_t NumaUnassigned = (size_t) -1;

// ---- small growable arrays --------------------------------------------------

static bool node_reserve(NumaNode *node, size_t want) {
    if (want <= node->capacity)
        return true;

    size_t capacity = node->capacity == 0 ? 8 : node->capacity;
    while (capacity < want)
        capacity *= 2;

    size_t *cpus = realloc(node->cpus, capacity * sizeof *cpus);
    if (cpus == nullptr)
        return false;

    node->cpus = cpus;
    node->capacity = capacity;
    return true;
}

// Insert CPU keeping the node's set ascending; treat a duplicate as a no-op success.
static bool node_insert_sorted(NumaNode *node, size_t cpu) {
    size_t i = 0;
    while (i < node->count && node->cpus[i] < cpu)
        ++i;
    if (i < node->count && node->cpus[i] == cpu)
        return true;

    if (!node_reserve(node, node->count + 1))
        return false;

    memmove(&node->cpus[i + 1], &node->cpus[i], (node->count - i) * sizeof *node->cpus);
    node->cpus[i] = cpu;
    node->count += 1;
    return true;
}

static bool nodes_reserve(NumaConfig *cfg, size_t want) {
    if (want <= cfg->node_capacity)
        return true;

    size_t capacity = cfg->node_capacity == 0 ? 4 : cfg->node_capacity;
    while (capacity < want)
        capacity *= 2;

    NumaNode *nodes = realloc(cfg->nodes, capacity * sizeof *nodes);
    if (nodes == nullptr)
        return false;

    cfg->nodes = nodes;
    cfg->node_capacity = capacity;
    return true;
}

static bool cpu_map_reserve(NumaConfig *cfg, size_t cpu) {
    if (cpu < cfg->cpu_map_len)
        return true;

    size_t len = cfg->cpu_map_len == 0 ? 64 : cfg->cpu_map_len;
    while (len <= cpu)
        len *= 2;

    size_t *map = realloc(cfg->node_by_cpu, len * sizeof *map);
    if (map == nullptr)
        return false;

    for (size_t i = cfg->cpu_map_len; i < len; ++i)
        map[i] = NumaUnassigned;

    cfg->node_by_cpu = map;
    cfg->cpu_map_len = len;
    return true;
}

// ---- NumaConfig -------------------------------------------------------------

void numa_config_init(NumaConfig *cfg) { memset(cfg, 0, sizeof *cfg); }

void numa_config_destroy(NumaConfig *cfg) {
    for (size_t i = 0; i < cfg->node_count; ++i)
        free(cfg->nodes[i].cpus);
    free(cfg->nodes);
    free(cfg->node_by_cpu);
    memset(cfg, 0, sizeof *cfg);
}

NumaAddStatus numa_config_add_cpu_to_node(NumaConfig *cfg, size_t node, size_t cpu) {
    if (cpu >= (size_t) NumaMaxCpus)
        return NUMA_ADD_CONFLICT;

    // Refuse ANY already-assigned CPU, including a re-add to the node it already holds.
    // Upstream's add_cpu_to_node opens with `if (is_cpu_assigned(c)) return false`
    // (numa.h:995), and from_string turns that false into a rejected policy string --
    // so treating a repeat as success would accept "NumaPolicy 0,0", which upstream
    // rejects, and silently under-count that node's CPUs against the thread it names.
    if (numa_config_is_cpu_assigned(cfg, cpu))
        return NUMA_ADD_CONFLICT;

    if (!nodes_reserve(cfg, node + 1) || !cpu_map_reserve(cfg, cpu))
        return NUMA_ADD_OOM;

    while (cfg->node_count <= node) {
        cfg->nodes[cfg->node_count] = (NumaNode) { nullptr, 0, 0 };
        cfg->node_count += 1;
    }

    if (!node_insert_sorted(&cfg->nodes[node], cpu))
        return NUMA_ADD_OOM;

    cfg->node_by_cpu[cpu] = node;
    cfg->assigned_cpus += 1;
    return NUMA_ADD_OK;
}

size_t numa_config_num_nodes(const NumaConfig *cfg) { return cfg->node_count; }

size_t numa_config_num_cpus_in_node(const NumaConfig *cfg, size_t node) {
    return node < cfg->node_count ? cfg->nodes[node].count : 0;
}

size_t numa_config_num_cpus(const NumaConfig *cfg) { return cfg->assigned_cpus; }

bool numa_config_is_cpu_assigned(const NumaConfig *cfg, size_t cpu) {
    return cpu < cfg->cpu_map_len && cfg->node_by_cpu[cpu] != NumaUnassigned;
}

const size_t *numa_config_node_cpus(const NumaConfig *cfg, size_t node, size_t *out_count) {
    if (node >= cfg->node_count) {
        *out_count = 0;
        return nullptr;
    }
    *out_count = cfg->nodes[node].count;
    return cfg->nodes[node].cpus;
}

void numa_config_remove_empty_nodes(NumaConfig *cfg) {
    size_t dst = 0;
    for (size_t src = 0; src < cfg->node_count; ++src) {
        if (cfg->nodes[src].count == 0) {
            free(cfg->nodes[src].cpus);
            continue;
        }
        if (dst != src) {
            cfg->nodes[dst] = cfg->nodes[src];
            for (size_t i = 0; i < cfg->nodes[dst].count; ++i)
                cfg->node_by_cpu[cfg->nodes[dst].cpus[i]] = dst;
        }
        ++dst;
    }
    cfg->node_count = dst;
}

// ---- index-list parsing ("0-3,8") -------------------------------------------

static bool parse_uint(const char *s, size_t len, size_t *out) {
    if (len == 0)
        return false;

    size_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        const size_t digit = (size_t) (s[i] - '0');
        if (value > ((size_t) -1 - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    *out = value;
    return true;
}

// Cap one "first-last" element the way upstream does, so a hostile "0-4000000000" costs
// nothing (numa.h:1053). The comparison is unsigned, so a REVERSED range wraps to a huge
// difference and fails the cap -- which is exactly how upstream drops "7-3".
enum { NumaMaxRangeIndices = 1 << 20 };

// Parse one comma element into the half-open-free range [*lo, *hi]. Return false when the
// element contributes NO indices; that is not an error, and the caller must skip it.
//
// Upstream's indices_from_shortened_string (numa.h:1033) never fails: an element it cannot
// read contributes nothing and the walk continues. Matching that is not pedantry --
// "0-1,7-3" is a config upstream accepts as node {0,1} and a rejecting parser turns into
// a refused NumaPolicy, so the two engines would run different topologies.
static bool parse_element(const char *s, size_t len, size_t *lo, size_t *hi) {
    const char *dash = memchr(s, '-', len);
    if (dash == nullptr) {
        if (!parse_uint(s, len, lo))
            return false;
        *hi = *lo;
        return true;
    }

    const size_t head = (size_t) (dash - s);
    const char *tail = dash + 1;
    const size_t tail_len = len - head - 1;

    // Refuse a third part. Upstream splits on '-' and skips anything but one or two parts,
    // so "1-2-3" contributes nothing rather than reading as 1-2.
    if (memchr(tail, '-', tail_len) != nullptr)
        return false;

    if (!parse_uint(s, head, lo) || !parse_uint(tail, tail_len, hi))
        return false;

    return *hi - *lo < (size_t) NumaMaxRangeIndices;
}

// Walk a comma-separated index list, calling SINK for each index. Skip empty elements, any
// whitespace (which /sys files carry as a trailing newline) and any element that parses to
// nothing. Return false only when SINK refuses -- that is an OOM or a topology conflict,
// which the caller must propagate.
static bool for_each_index(
  const char *s, size_t len, bool (*sink)(void *ctx, size_t index), void *ctx, bool *out_any) {
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i != len && s[i] != ',')
            continue;

        const char *element = s + start;
        size_t element_len = i - start;
        while (element_len != 0 && (unsigned char) *element <= ' ') {
            ++element;
            --element_len;
        }
        while (element_len != 0 && (unsigned char) element[element_len - 1] <= ' ')
            --element_len;
        start = i + 1;

        if (element_len == 0)
            continue;

        size_t lo = 0;
        size_t hi = 0;
        if (!parse_element(element, element_len, &lo, &hi))
            continue;

        for (size_t index = lo; index <= hi; ++index) {
            if (!sink(ctx, index))
                return false;
            if (out_any != nullptr)
                *out_any = true;
        }
    }
    return true;
}

typedef struct {
    NumaConfig *cfg;
    size_t node;
} AddSink;

static bool add_sink(void *ctx, size_t index) {
    AddSink *s = (AddSink *) ctx;
    return numa_config_add_cpu_to_node(s->cfg, s->node, index) == NUMA_ADD_OK;
}

bool numa_config_from_string(NumaConfig *out, const char *s, size_t len) {
    NumaConfig cfg;
    numa_config_init(&cfg);

    size_t node = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i != len && s[i] != ':')
            continue;

        AddSink sink = { &cfg, node };
        bool any = false;
        if (!for_each_index(s + start, i - start, add_sink, &sink, &any)) {
            numa_config_destroy(&cfg);
            return false;
        }
        start = i + 1;

        // Skip an empty node segment without advancing the node index.
        if (any)
            node += 1;
    }

    // Refuse a string that named no node at all. Upstream returns nullopt on `n == 0`
    // (numa.h:686), and the caller keeps the previous topology. Accepting it would install
    // a config with zero nodes, on which distribute_threads and suggests_binding both
    // divide by a node count of zero.
    if (node == 0) {
        numa_config_destroy(&cfg);
        return false;
    }

    cfg.custom_affinity = true;
    *out = cfg;
    return true;
}

// ---- system topology --------------------------------------------------------

// Hold the process affinity mask, or a "everything is allowed" marker when the syscall is
// unavailable (a seccomp sandbox, a filtered container).
typedef struct {
    cpu_set_t mask;
    bool restricted;
} AffinityMask;

static void affinity_mask_read(AffinityMask *out, bool respect_process_affinity) {
    CPU_ZERO(&out->mask);
    out->restricted = false;

    if (!respect_process_affinity)
        return;

    if (sched_getaffinity(0, sizeof out->mask, &out->mask) != 0) {
        // Check the return: dropping it leaves the mask all-zero, and the bit walk below
        // would then report an EMPTY cpu set as if the process were bound to nothing.
        CPU_ZERO(&out->mask);
        return;
    }

    out->restricted = true;
}

static bool affinity_allows(const AffinityMask *mask, size_t cpu) {
    if (!mask->restricted)
        return true;
    if (cpu >= (size_t) CPU_SETSIZE)
        return false;
    // Silence -Wsign-conversion inside glibc's CPU_ISSET, which divides its own `int`
    // parameter by a size_t. The warning is in the macro's expansion, not in the argument:
    // the index is already range-checked above.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
    return CPU_ISSET((int) cpu, &mask->mask) != 0;
#pragma GCC diagnostic pop
}

// Read a whole /sys file. Return a malloc'd NUL-terminated buffer, or nullptr when the
// file is absent or unreadable.
static char *read_file_to_string(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
        return nullptr;

    size_t capacity = 256;
    size_t len = 0;
    char *buf = malloc(capacity);
    if (buf == nullptr) {
        (void) fclose(f);
        return nullptr;
    }

    for (;;) {
        if (len + 1 == capacity) {
            capacity *= 2;
            char *grown = realloc(buf, capacity);
            if (grown == nullptr) {
                free(buf);
                (void) fclose(f);
                return nullptr;
            }
            buf = grown;
        }

        const size_t got = fread(buf + len, 1, capacity - len - 1, f);
        len += got;
        if (got == 0)
            break;
    }

    (void) fclose(f);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

// Build the single-node fallback: one node holding every allowed CPU. This is the shape a
// host with no /sys NUMA directory reports, and the shape `NumaPolicy none` asks for.
bool numa_config_from_system_single(NumaConfig *out, bool respect_process_affinity) {
    AffinityMask mask;
    affinity_mask_read(&mask, respect_process_affinity);

    NumaConfig cfg;
    numa_config_init(&cfg);

    const size_t count = thread_hardware_concurrency();
    size_t added = 0;
    // Walk past `count` when the mask is sparse: an affinity mask of {8..15} has 8 allowed
    // CPUs whose indices all exceed 8, so stopping at `count` would find none of them.
    const size_t limit = mask.restricted ? (size_t) CPU_SETSIZE : count;
    for (size_t cpu = 0; cpu < limit && added < count; ++cpu) {
        if (!affinity_allows(&mask, cpu))
            continue;
        if (numa_config_add_cpu_to_node(&cfg, 0, cpu) == NUMA_ADD_OOM) {
            numa_config_destroy(&cfg);
            return false;
        }
        ++added;
    }

    // Guarantee a non-empty node even where the mask reported nothing usable.
    if (added == 0 && numa_config_add_cpu_to_node(&cfg, 0, 0) == NUMA_ADD_OOM) {
        numa_config_destroy(&cfg);
        return false;
    }

    *out = cfg;
    return true;
}

// Bound the /sys node id the same way NumaMaxCpus bounds a CPU index: the id is used
// directly as the node index (upstream numa.h:1108 does the same), so a corrupt `online`
// file must not size the node array.
enum { NumaMaxNodes = 4096 };

typedef struct {
    NumaConfig *cfg;
    const AffinityMask *mask;
    size_t node;
    bool failed;
} SysNodeSink;

static bool sys_cpu_sink(void *ctx, size_t index) {
    SysNodeSink *s = (SysNodeSink *) ctx;
    if (!affinity_allows(s->mask, index))
        return true;  // an excluded CPU is not an error, just not ours
    const NumaAddStatus status = numa_config_add_cpu_to_node(s->cfg, s->node, index);
    if (status == NUMA_ADD_OOM)
        s->failed = true;
    return status != NUMA_ADD_OOM;
}

static bool sys_node_sink(void *ctx, size_t index) {
    SysNodeSink *s = (SysNodeSink *) ctx;

    if (index >= (size_t) NumaMaxNodes)
        return false;

    // Use the /sys node id as the node index. Some ids may hold no allowed CPU at all;
    // remove_empty_nodes closes those gaps once the whole topology is read.
    s->node = index;

    char path[64];
    (void) snprintf(path, sizeof path, "/sys/devices/system/node/node%zu/cpulist", index);

    size_t len = 0;
    char *cpu_ids = read_file_to_string(path, &len);
    if (cpu_ids == nullptr)
        return false;  // bail only when the file does not exist; an empty node is fine

    const bool ok = for_each_index(cpu_ids, len, sys_cpu_sink, s, nullptr);
    free(cpu_ids);
    return ok && !s->failed;
}

// Read the system's own NUMA partition from /sys/devices/system/node. This is upstream's
// from_system_numa (numa.h:1075) -- the raw topology, before any L3 subdivision.
static bool from_system_numa(NumaConfig *out, bool respect_process_affinity) {
    AffinityMask mask;
    affinity_mask_read(&mask, respect_process_affinity);

    size_t len = 0;
    char *node_ids = read_file_to_string("/sys/devices/system/node/online", &len);
    if (node_ids == nullptr || len == 0) {
        free(node_ids);
        return numa_config_from_system_single(out, respect_process_affinity);
    }

    NumaConfig cfg;
    numa_config_init(&cfg);

    SysNodeSink sink = { &cfg, &mask, 0, false };
    const bool ok = for_each_index(node_ids, len, sys_node_sink, &sink, nullptr);
    free(node_ids);

    if (!ok) {
        numa_config_destroy(&cfg);
        return numa_config_from_system_single(out, respect_process_affinity);
    }

    numa_config_remove_empty_nodes(&cfg);
    if (cfg.node_count == 0) {
        numa_config_destroy(&cfg);
        return numa_config_from_system_single(out, respect_process_affinity);
    }

    *out = cfg;
    return true;
}

// ---- the L3-aware partition -------------------------------------------------
//
// Upstream's DEFAULT policy is BundledL3Policy{32} (engine.cpp:58), and from_system tries
// the L3-aware config FIRST, falling back to the raw NUMA partition only when no L3
// domain can be read (numa.h:583-598). This is not a refinement: on a chiplet CPU one
// system NUMA node spans several L3 domains, so the raw partition reports ONE node where
// upstream reports several -- a different thread distribution, a different number of
// shared-history banks, and a different bind decision for the same `Threads` value.

// Hold one L3 domain: the CPUs sharing an L3 cache, and the system NUMA node they sit on.
// Reuse NumaNode as the CPU set -- node_insert_sorted gives the ascending, duplicate-free
// set upstream's std::set does, which is what makes the merge below order-independent.
typedef struct {
    size_t system_node;
    NumaNode cpus;
} L3Domain;

typedef struct {
    L3Domain *items;
    size_t count;
    size_t capacity;
} L3DomainList;

static void l3_domains_destroy(L3DomainList *list) {
    for (size_t i = 0; i < list->count; ++i)
        free(list->items[i].cpus.cpus);
    free(list->items);
    memset(list, 0, sizeof *list);
}

static bool l3_domains_push(L3DomainList *list, const L3Domain *domain) {
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        L3Domain *grown = realloc(list->items, capacity * sizeof *grown);
        if (grown == nullptr)
            return false;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = *domain;
    return true;
}

// Collect the CPUs of one shared_cpu_list into a domain.
typedef struct {
    const NumaConfig *system_cfg;
    const AffinityMask *mask;
    L3Domain *domain;
    bool *seen;
    size_t seen_len;
    bool failed;
} L3CpuSink;

static bool l3_cpu_sink(void *ctx, size_t index) {
    L3CpuSink *s = (L3CpuSink *) ctx;

    // Mark the CPU seen whether or not it is ours: upstream inserts into seenCpus inside
    // the loop over the whole sibling list, so a CPU excluded by affinity still stops the
    // outer walk from re-reading the same L3 domain.
    if (index < s->seen_len)
        s->seen[index] = true;

    if (!affinity_allows(s->mask, index) || !numa_config_is_cpu_assigned(s->system_cfg, index))
        return true;

    s->domain->system_node = s->system_cfg->node_by_cpu[index];
    if (!node_insert_sorted(&s->domain->cpus, index)) {
        s->failed = true;
        return false;
    }
    return true;
}

// Merge the domains of each system node pairwise while a pair still fits in BUNDLE_SIZE,
// then emit one config node per surviving domain. Upstream: numa.h:1246 (from_l3_info).
static bool from_l3_info(NumaConfig *out, L3DomainList *domains, size_t bundle_size) {
    NumaConfig cfg;
    numa_config_init(&cfg);

    size_t node = 0;

    size_t highest_system_node = 0;
    for (size_t i = 0; i < domains->count; ++i)
        if (domains->items[i].system_node > highest_system_node)
            highest_system_node = domains->items[i].system_node;

    size_t *group = malloc(domains->count * sizeof *group);
    if (group == nullptr) {
        numa_config_destroy(&cfg);
        return false;
    }

    // Walk the system nodes in ascending order, as upstream's std::map does.
    for (size_t system_node = 0; system_node <= highest_system_node; ++system_node) {
        // Gather this system node's domains, keeping discovery order.
        size_t group_count = 0;
        for (size_t i = 0; i < domains->count; ++i)
            if (domains->items[i].system_node == system_node)
                group[group_count++] = i;

        if (group_count == 0)
            continue;

        // Scan through pairs and merge them, repeating until a pass changes nothing.
        // Upstream does not decrement j after an erase, so one pass merges alternating
        // pairs and the outer loop catches the rest.
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t j = 0; j + 1 < group_count; ++j) {
                NumaNode *a = &domains->items[group[j]].cpus;
                const NumaNode *b = &domains->items[group[j + 1]].cpus;
                if (a->count + b->count > bundle_size)
                    continue;

                changed = true;
                for (size_t k = 0; k < b->count; ++k) {
                    if (!node_insert_sorted(a, b->cpus[k])) {
                        free(group);
                        numa_config_destroy(&cfg);
                        return false;
                    }
                }
                memmove(&group[j + 1], &group[j + 2], (group_count - j - 2) * sizeof *group);
                --group_count;
            }
        }

        for (size_t j = 0; j < group_count; ++j) {
            const NumaNode *cpus = &domains->items[group[j]].cpus;
            for (size_t k = 0; k < cpus->count; ++k) {
                if (numa_config_add_cpu_to_node(&cfg, node, cpus->cpus[k]) == NUMA_ADD_OOM) {
                    free(group);
                    numa_config_destroy(&cfg);
                    return false;
                }
            }
            node += 1;
        }
    }

    free(group);

    if (cfg.node_count == 0) {
        numa_config_destroy(&cfg);
        return false;
    }

    *out = cfg;
    return true;
}

// Build the L3-aware partition, or report that the host exposes no L3 topology.
static bool
try_get_l3_aware_config(NumaConfig *out, bool respect_process_affinity, size_t bundle_size) {
    AffinityMask mask;
    affinity_mask_read(&mask, respect_process_affinity);

    // Read the raw NUMA partition first: it says which system node each L3 domain sits on,
    // which is what keeps a merged bundle inside one node's memory.
    NumaConfig system_cfg;
    if (!from_system_numa(&system_cfg, respect_process_affinity))
        return false;

    bool *seen = calloc(system_cfg.cpu_map_len == 0 ? 1 : system_cfg.cpu_map_len, sizeof *seen);
    if (seen == nullptr) {
        numa_config_destroy(&system_cfg);
        return false;
    }

    L3DomainList domains = { nullptr, 0, 0 };
    bool ok = true;

    for (size_t cpu = 0; cpu < system_cfg.cpu_map_len && ok; ++cpu) {
        if (!numa_config_is_cpu_assigned(&system_cfg, cpu) || seen[cpu])
            continue;

        char path[96];
        (void) snprintf(path, sizeof path,
                        "/sys/devices/system/cpu/cpu%zu/cache/index3/shared_cpu_list", cpu);

        size_t len = 0;
        char *siblings = read_file_to_string(path, &len);
        if (siblings == nullptr || len == 0) {
            // A CPU with no index3 cache entry contributes no domain. Upstream leaves it
            // unseen too, so a later CPU whose sibling list names it can still claim it.
            free(siblings);
            continue;
        }

        L3Domain domain = { 0, { nullptr, 0, 0 } };
        L3CpuSink sink = { &system_cfg, &mask, &domain, seen, system_cfg.cpu_map_len, false };
        ok = for_each_index(siblings, len, l3_cpu_sink, &sink, nullptr) && !sink.failed;
        free(siblings);

        if (!ok || domain.cpus.count == 0) {
            free(domain.cpus.cpus);
            continue;
        }

        if (!l3_domains_push(&domains, &domain)) {
            free(domain.cpus.cpus);
            ok = false;
        }
    }

    free(seen);
    numa_config_destroy(&system_cfg);

    if (!ok || domains.count == 0) {
        l3_domains_destroy(&domains);
        return false;
    }

    const bool built = from_l3_info(out, &domains, bundle_size);
    l3_domains_destroy(&domains);
    return built;
}

bool numa_config_from_system(NumaConfig *out, bool respect_process_affinity) {
    // Try the L3-aware partition FIRST, as upstream's from_system does under its default
    // BundledL3Policy{32} (numa.h:583, engine.cpp:58). Falling straight through to the raw
    // NUMA read is what makes a chiplet host report one node where upstream reports one
    // per L3 bundle.
    if (try_get_l3_aware_config(out, respect_process_affinity, NumaL3BundleSize))
        return true;

    return from_system_numa(out, respect_process_affinity);
}

// ---- distribution and the bind decision -------------------------------------

bool numa_config_distribute_threads(const NumaConfig *cfg, size_t num_threads, size_t *out_nodes) {
    if (num_threads == 0)
        return true;

    if (cfg->node_count <= 1) {
        memset(out_nodes, 0, num_threads * sizeof *out_nodes);
        return true;
    }

    size_t *occupation = calloc(cfg->node_count, sizeof *occupation);
    if (occupation == nullptr)
        return false;

    for (size_t t = 0; t < num_threads; ++t) {
        size_t best = 0;
        // Keep the fill ratio in `float`: a wider accumulator would move the ties and
        // hand a different node to a thread.
        float best_fill = __builtin_inff();
        for (size_t n = 0; n < cfg->node_count; ++n) {
            const float fill = (float) (occupation[n] + 1) / (float) cfg->nodes[n].count;
            if (fill < best_fill) {
                best = n;
                best_fill = fill;
            }
        }
        out_nodes[t] = best;
        occupation[best] += 1;
    }

    free(occupation);
    return true;
}

bool numa_config_suggests_binding_threads(const NumaConfig *cfg, size_t num_threads) {
    // A mismatch between the user's affinity and the OS's means binding is required to
    // keep threads on the correct processors.
    if (cfg->custom_affinity)
        return true;

    // A single thread cannot be distributed, so never bind it.
    if (num_threads <= 1)
        return false;

    size_t largest_node_size = 0;
    for (size_t n = 0; n < cfg->node_count; ++n) {
        if (cfg->nodes[n].count > largest_node_size)
            largest_node_size = cfg->nodes[n].count;
    }

    // Treat a node holding <= 60% of the largest node's CPUs as small. Keep the negated
    // comparison: with no CPUs at all the ratio is NaN, and `!(NaN <= t)` counts the node,
    // which is what upstream does.
    const double small_node_threshold = 0.6;
    size_t num_not_small_nodes = 0;
    for (size_t n = 0; n < cfg->node_count; ++n) {
        const double ratio = (double) cfg->nodes[n].count / (double) largest_node_size;
        if (!(ratio <= small_node_threshold))
            num_not_small_nodes += 1;
    }

    return (num_threads > largest_node_size / 2 || num_threads >= num_not_small_nodes * 4)
        && cfg->node_count > 1;
}

bool numa_config_bind_current_thread(const NumaConfig *cfg, size_t node) {
    if (node >= cfg->node_count)
        return false;
    return thread_set_affinity(cfg->nodes[node].cpus, cfg->nodes[node].count);
}

// ---- the affinity string ----------------------------------------------------

char *numa_config_string(void) {
    AffinityMask mask;
    affinity_mask_read(&mask, true);

    // Report every CPU as the fallback: used when the affinity syscall is unavailable.
    if (!mask.restricted) {
        const size_t n = thread_hardware_concurrency();
        char *buf = malloc(48);
        if (buf == nullptr)
            return nullptr;
        if (n <= 1)
            (void) snprintf(buf, 48, "0");
        else
            (void) snprintf(buf, 48, "0-%zu", n - 1);
        return buf;
    }

    size_t capacity = 128;
    size_t len = 0;
    char *buf = malloc(capacity);
    if (buf == nullptr)
        return nullptr;
    buf[0] = '\0';

    bool first = true;
    for (size_t i = 0; i < (size_t) CPU_SETSIZE;) {
        if (!affinity_allows(&mask, i)) {
            ++i;
            continue;
        }

        size_t j = i;
        while (j + 1 < (size_t) CPU_SETSIZE && affinity_allows(&mask, j + 1))
            ++j;

        char segment[48];
        int written;
        if (j == i)
            written = snprintf(segment, sizeof segment, "%s%zu", first ? "" : ",", i);
        else
            written = snprintf(segment, sizeof segment, "%s%zu-%zu", first ? "" : ",", i, j);
        first = false;

        const size_t need = len + (size_t) (written < 0 ? 0 : written) + 1;
        if (need > capacity) {
            while (capacity < need)
                capacity *= 2;
            char *grown = realloc(buf, capacity);
            if (grown == nullptr) {
                free(buf);
                return nullptr;
            }
            buf = grown;
        }
        memcpy(buf + len, segment, (size_t) (written < 0 ? 0 : written) + 1);
        len += (size_t) (written < 0 ? 0 : written);

        i = j + 1;
    }

    return buf;
}
