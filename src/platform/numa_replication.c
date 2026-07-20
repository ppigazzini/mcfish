// The NUMA replication context: the registry of replicated objects (the NNUE net) and the
// policy switches (system / hardware / none / explicit string) that re-partition and
// re-replicate them. A cohesive unit over the public NumaReplicationContext, using the
// public numa_config_* API. Golden: upstream `numa.h` (NumaReplicationContext).

#include "numa.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

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

void numa_context_set_hardware(NumaReplicationContext *ctx) {
    // Read the topology WITHOUT the process affinity mask, as upstream's `hardware` does
    // (engine.cpp:227). This is the one thing that separates it from `system`: a run
    // pinned to half the box reports the whole box here.
    NumaConfig cfg;
    if (!numa_config_from_system(&cfg, false))
        return;
    cfg.custom_affinity = true;  // numa.h:657 -- opting out of the mask IS a custom config
    numa_context_set_config(ctx, &cfg);
}

void numa_context_set_none(NumaReplicationContext *ctx) {
    // "none" means one node holding every processor: bind nothing, replicate from node 0.
    // Ignore the affinity mask, as upstream's default-constructed NumaConfig does
    // (numa.h:535) -- it adds the range 0..hardware_concurrency-1 unconditionally.
    NumaConfig cfg;
    if (!numa_config_from_system_single(&cfg, false))
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

// Carry one execute-on-node request to the throwaway thread that runs it.
typedef struct {
    const NumaConfig *config;
    size_t node;
    void (*callback)(void *ctx);
    void *callback_ctx;
} ExecuteOnNodeJob;

static void *execute_on_node_entry(void *arg) {
    const ExecuteOnNodeJob *job = (const ExecuteOnNodeJob *) arg;
    (void) numa_config_bind_current_thread(job->config, job->node);
    job->callback(job->callback_ctx);
    return nullptr;
}

void numa_execute_on_node(const NumaReplicationContext *ctx,
                          size_t node,
                          void (*callback)(void *ctx),
                          void *callback_ctx) {
    // Run on a THROWAWAY thread and join it, as upstream's execute_on_numa_node does
    // (numa.h:957). The binding is the point of the call, and it must not survive the
    // call: binding the caller instead confines whoever asked -- the UCI thread during a
    // pool rebuild -- to one node's CPUs for the rest of the process, and every later
    // allocation it makes then first-touches that one node.
    ExecuteOnNodeJob job = { &ctx->config, node, callback, callback_ctx };

    pthread_t handle;
    if (pthread_create(&handle, nullptr, execute_on_node_entry, &job) != 0) {
        // Fall back to running unbound on the caller rather than skipping the work: the
        // callback is a construction step, and its absence is a null reference later,
        // where a wrongly-placed allocation is only slower.
        callback(callback_ctx);
        return;
    }

    (void) pthread_join(handle, nullptr);
}
