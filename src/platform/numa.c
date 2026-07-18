// Define _GNU_SOURCE before any libc header: cpu_set_t, CPU_SET/CPU_ISSET and
// sched_getaffinity sit behind glibc's __USE_GNU guard, which -D_POSIX_C_SOURCE=200809L
// alone does not open. See PORT_NOTES_platform.md.
#define _GNU_SOURCE

#include "numa.h"

#include "thread.h"

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

    if (cpu < cfg->cpu_map_len && cfg->node_by_cpu[cpu] != NumaUnassigned)
        return cfg->node_by_cpu[cpu] == node ? NUMA_ADD_OK : NUMA_ADD_CONFLICT;

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

// Parse one comma element: "5" or "3-7". Reject hi < lo, as upstream does.
static bool parse_range(const char *s, size_t len, size_t *lo, size_t *hi) {
    const char *dash = memchr(s, '-', len);
    if (dash == nullptr) {
        if (!parse_uint(s, len, lo))
            return false;
        *hi = *lo;
        return true;
    }

    const size_t head = (size_t) (dash - s);
    if (!parse_uint(s, head, lo) || !parse_uint(dash + 1, len - head - 1, hi))
        return false;

    return *hi >= *lo;
}

// Walk a comma-separated index list, calling SINK for each index. Skip empty elements and
// any whitespace, which /sys files carry as a trailing newline. Return false on a
// malformed element or when SINK refuses.
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
        if (!parse_range(element, element_len, &lo, &hi))
            return false;

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
static bool from_system_single(NumaConfig *out, bool respect_process_affinity) {
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

bool numa_config_from_system(NumaConfig *out, bool respect_process_affinity) {
    AffinityMask mask;
    affinity_mask_read(&mask, respect_process_affinity);

    size_t len = 0;
    char *node_ids = read_file_to_string("/sys/devices/system/node/online", &len);
    if (node_ids == nullptr || len == 0) {
        free(node_ids);
        return from_system_single(out, respect_process_affinity);
    }

    NumaConfig cfg;
    numa_config_init(&cfg);

    SysNodeSink sink = { &cfg, &mask, 0, false };
    const bool ok = for_each_index(node_ids, len, sys_node_sink, &sink, nullptr);
    free(node_ids);

    if (!ok) {
        numa_config_destroy(&cfg);
        return from_system_single(out, respect_process_affinity);
    }

    numa_config_remove_empty_nodes(&cfg);
    if (cfg.node_count == 0) {
        numa_config_destroy(&cfg);
        return from_system_single(out, respect_process_affinity);
    }

    *out = cfg;
    return true;
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
        // Keep the fill ratio in `float`, as the port source does: a wider accumulator
        // would move the ties and hand a different node to a thread.
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
    // which is what the port source and upstream both do.
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

// ---- replication ------------------------------------------------------------

void numa_context_init(NumaReplicationContext *ctx, NumaConfig *config) {
    memset(ctx, 0, sizeof *ctx);
    ctx->config = *config;
    numa_config_init(config);
}

void numa_context_destroy(NumaReplicationContext *ctx) {
    free(ctx->tracked);
    numa_config_destroy(&ctx->config);
    memset(ctx, 0, sizeof *ctx);
}

static size_t tracked_index_of(const NumaReplicationContext *ctx, const NumaReplicatedBase *obj) {
    for (size_t i = 0; i < ctx->tracked_count; ++i) {
        if (ctx->tracked[i] == obj)
            return i;
    }
    return NumaUnassigned;
}

bool numa_context_attach(NumaReplicationContext *ctx, NumaReplicatedBase *obj) {
    if (tracked_index_of(ctx, obj) != NumaUnassigned)
        return false;  // require obj not already tracked

    if (ctx->tracked_count == ctx->tracked_capacity) {
        const size_t capacity = ctx->tracked_capacity == 0 ? 4 : ctx->tracked_capacity * 2;
        NumaReplicatedBase **grown = realloc(ctx->tracked, capacity * sizeof *grown);
        if (grown == nullptr)
            return false;
        ctx->tracked = grown;
        ctx->tracked_capacity = capacity;
    }

    obj->context = ctx;
    ctx->tracked[ctx->tracked_count++] = obj;
    return true;
}

void numa_context_detach(NumaReplicationContext *ctx, NumaReplicatedBase *obj) {
    const size_t i = tracked_index_of(ctx, obj);
    if (i == NumaUnassigned)
        return;
    ctx->tracked[i] = ctx->tracked[--ctx->tracked_count];
}

void numa_context_move_attached(NumaReplicationContext *ctx,
                                NumaReplicatedBase *old_obj,
                                NumaReplicatedBase *new_obj) {
    const size_t i = tracked_index_of(ctx, old_obj);
    if (i == NumaUnassigned)
        return;
    ctx->tracked[i] = new_obj;
    new_obj->context = ctx;
}

void numa_context_set_config(NumaReplicationContext *ctx, NumaConfig *config) {
    numa_config_destroy(&ctx->config);
    ctx->config = *config;
    numa_config_init(config);

    for (size_t i = 0; i < ctx->tracked_count; ++i)
        ctx->tracked[i]->on_config_changed(ctx->tracked[i]);
}

const NumaConfig *numa_context_config(const NumaReplicationContext *ctx) { return &ctx->config; }

size_t numa_context_tracked_count(const NumaReplicationContext *ctx) { return ctx->tracked_count; }

void numa_context_set_system(NumaReplicationContext *ctx) {
    NumaConfig cfg;
    if (!numa_config_from_system(&cfg, true))
        return;  // keep the previous topology rather than dropping to none on OOM
    numa_context_set_config(ctx, &cfg);
}

void numa_context_set_hardware(NumaReplicationContext *ctx) { numa_context_set_system(ctx); }

void numa_context_set_none(NumaReplicationContext *ctx) {
    // "none" means one node holding every processor: bind nothing, replicate from node 0.
    NumaConfig cfg;
    if (!from_system_single(&cfg, true))
        return;
    numa_context_set_config(ctx, &cfg);
}

bool numa_context_set_from_string(NumaReplicationContext *ctx, const char *s, size_t len) {
    NumaConfig cfg;
    if (!numa_config_from_string(&cfg, s, len))
        return false;
    numa_context_set_config(ctx, &cfg);
    return true;
}

size_t numa_context_node_count(const NumaReplicationContext *ctx) { return ctx->config.node_count; }

size_t numa_context_cpus_in_node(const NumaReplicationContext *ctx, size_t node) {
    return numa_config_num_cpus_in_node(&ctx->config, node);
}

bool numa_suggests_binding_threads(const NumaReplicationContext *ctx, size_t num_threads) {
    return numa_config_suggests_binding_threads(&ctx->config, num_threads);
}

size_t numa_distribute_threads_among_nodes(const NumaReplicationContext *ctx,
                                           size_t requested,
                                           size_t *out_nodes) {
    if (!numa_config_distribute_threads(&ctx->config, requested, out_nodes)) {
        // Degrade to node 0 rather than abort a search on OOM.
        memset(out_nodes, 0, requested * sizeof *out_nodes);
    }
    return ctx->config.node_count > 1 ? ctx->config.node_count : 1;
}

void numa_execute_on_node(const NumaReplicationContext *ctx,
                          size_t node,
                          void (*callback)(void *ctx),
                          void *callback_ctx) {
    (void) numa_config_bind_current_thread(&ctx->config, node);
    callback(callback_ctx);
}
