// Own the NUMA topology model and the replication registry built on it.
//
// A NumaConfig is a list of nodes, each an ascending, duplicate-free set of CPU indices,
// plus the reverse cpu->node index. The invariant is that a CPU belongs to at most one
// node: numa_config_add_cpu_to_node refuses a re-assignment rather than silently moving
// the CPU, because a topology where one CPU appears twice makes every thread-distribution
// answer arbitrary.
//
// A NumaReplicationContext owns one config and a registry of replicated objects (the NNUE
// network is the live one). Replacing the config notifies every registered object to
// re-replicate; that notification is the whole point of the registry, and skipping it is
// how a NumaPolicy change becomes a silent no-op.
//
// Read the topology from /sys/devices/system/node rather than linking libnuma, so the
// build keeps its no-dependencies property. Fail soft everywhere: no /sys, no nodes, a
// restricted affinity mask or an unparseable policy string all degrade to one node
// holding every allowed CPU, which is a correct single-node run.
//
// Upstream: numa.h:410 (add_cpu_to_node), numa.h:660 (from_string), numa.h:1075
// (from_system_numa), numa.h:722 (distribute_threads_among_numa_nodes), numa.h:756
// (suggests_binding_threads), numa.h:1290 (NumaReplicationContext).
// Port source: zfish src/platform/numa.zig, numa/config.zig, numa/replication.zig.

#ifndef MCFISH_NUMA_H
#define MCFISH_NUMA_H

#include <stdbool.h>
#include <stddef.h>

// Bound the CPU index the array-backed cpu->node map will accept. Upstream's map is a
// hash and needs no bound; here a hostile "NumaPolicy 0-4000000000" would otherwise ask
// for a multi-gigabyte index. Reject beyond this instead, which reads back to the caller
// as a malformed policy string.
enum { NumaMaxCpus = 65536 };

// Bundle L3 domains up to this many CPUs when partitioning the system. This is upstream's
// DEFAULT policy, BundledL3Policy{32} (engine.cpp:58), and it is what `auto`, `system` and
// `hardware` all resolve to -- SystemNumaPolicy is used only for the raw partition the L3
// pass reads to learn which system node each domain sits on.
enum { NumaL3BundleSize = 32 };

typedef struct {
    size_t *cpus;  // ascending, unique
    size_t count;
    size_t capacity;
} NumaNode;

typedef struct {
    NumaNode *nodes;
    size_t node_count;
    size_t node_capacity;

    // Map cpu index -> node index, NumaUnassigned for a CPU on no node.
    size_t *node_by_cpu;
    size_t cpu_map_len;
    size_t assigned_cpus;

    // Flag that the topology came from a user "NumaPolicy" string rather than the system;
    // force thread binding (numa.h:768).
    bool custom_affinity;
} NumaConfig;

// Report the outcome of an add: a conflict is the caller's error, an OOM is the host's.
typedef enum { NUMA_ADD_OK, NUMA_ADD_CONFLICT, NUMA_ADD_OOM } NumaAddStatus;

// Sentinel stored in node_by_cpu for an unassigned CPU.
extern const size_t NumaUnassigned;

void numa_config_init(NumaConfig *cfg);
void numa_config_destroy(NumaConfig *cfg);

// Add CPU to NODE. Create any missing lower nodes, keep the node's set ascending and
// unique, and treat re-adding a CPU to the node it already holds as a no-op success.
NumaAddStatus numa_config_add_cpu_to_node(NumaConfig *cfg, size_t node, size_t cpu);

size_t numa_config_num_nodes(const NumaConfig *cfg);
size_t numa_config_num_cpus_in_node(const NumaConfig *cfg, size_t node);
size_t numa_config_num_cpus(const NumaConfig *cfg);
bool numa_config_is_cpu_assigned(const NumaConfig *cfg, size_t cpu);

// Return node NODE's CPU list, or nullptr when NODE is out of range. The pointer is owned
// by CFG and is invalidated by any further add.
const size_t *numa_config_node_cpus(const NumaConfig *cfg, size_t node, size_t *out_count);

// Drop every empty node, closing the gaps. /sys can report a node with no allowed CPUs
// once the affinity mask is applied, and an empty node poisons the fill-ratio arithmetic
// in distribute and suggests_binding. Upstream: numa.h:652.
void numa_config_remove_empty_nodes(NumaConfig *cfg);

// Parse a "NumaPolicy" string: nodes separated by ':', each a comma list of CPU indices
// or ranges, e.g. "0-3,8:4-7" -> node0 {0,1,2,3,8}, node1 {4,5,6,7}. Skip empty node
// segments without advancing the node index. Return false and leave OUT untouched on a
// malformed string -- the caller must then REFUSE the option and keep the previous config,
// as upstream does (engine.cpp:236).
bool numa_config_from_string(NumaConfig *out, const char *s, size_t len);

// Build the topology from /sys/devices/system/node. Restrict to the process affinity mask
// when RESPECT_PROCESS_AFFINITY. Fall back to a single node holding every allowed CPU
// when /sys is absent or unreadable. Never fails other than on OOM.
bool numa_config_from_system(NumaConfig *out, bool respect_process_affinity);

// Assign each of NUM_THREADS threads to a node, writing NUM_THREADS entries into
// OUT_NODES. Place greedily on the node with the lowest (occupation+1)/size fill ratio;
// a single node takes everything. Return false only on OOM.
bool numa_config_distribute_threads(const NumaConfig *cfg, size_t num_threads, size_t *out_nodes);

// Decide whether to bind threads to nodes. Bind when the affinity is user-set; never bind
// a single thread; otherwise bind once the threads no longer fit comfortably in one node.
// Mirror upstream numa.h:756-794 exactly -- the arithmetic here decides whether the
// DEFAULT policy binds at all.
bool numa_config_suggests_binding_threads(const NumaConfig *cfg, size_t num_threads);

// Confine the CALLING thread to node NODE's CPUs. Return false when NODE is out of range
// or the host refuses; the thread then keeps its inherited affinity.
bool numa_config_bind_current_thread(const NumaConfig *cfg, size_t node);

// Render the process affinity mask as comma-joined ranges ("0-7,16-23"), malloc'd and
// NUL-terminated; the caller frees. Return nullptr only on OOM.
char *numa_config_string(void);

// ---- replication ------------------------------------------------------------

typedef struct NumaReplicationContext NumaReplicationContext;

// Embed this registry hook in every replicated wrapper. It carries a function pointer
// rather than a vtable, so a replicated object needs no inheritance.
typedef struct NumaReplicatedBase {
    NumaReplicationContext *context;
    // Re-replicate from node 0 after a config change.
    void (*on_config_changed)(struct NumaReplicatedBase *self);
} NumaReplicatedBase;

struct NumaReplicationContext {
    NumaConfig config;
    NumaReplicatedBase **tracked;
    size_t tracked_count;
    size_t tracked_capacity;
};

// Take ownership of CONFIG. The caller must not destroy it afterwards; it is zeroed on
// return so a stale destroy is harmless.
void numa_context_init(NumaReplicationContext *ctx, NumaConfig *config);
void numa_context_destroy(NumaReplicationContext *ctx);

// Register OBJ. Return false on OOM, or when OBJ is already tracked.
bool numa_context_attach(NumaReplicationContext *ctx, NumaReplicatedBase *obj);
void numa_context_detach(NumaReplicationContext *ctx, NumaReplicatedBase *obj);

// Move a registration OLD_OBJ (possibly already invalid) -> NEW_OBJ, same registry slot.
void numa_context_move_attached(NumaReplicationContext *ctx,
                                NumaReplicatedBase *old_obj,
                                NumaReplicatedBase *new_obj);

// Replace the config, taking ownership of CONFIG, and notify every tracked object to
// re-replicate. The notification is what makes a NumaPolicy change take effect.
void numa_context_set_config(NumaReplicationContext *ctx, NumaConfig *config);

const NumaConfig *numa_context_config(const NumaReplicationContext *ctx);
size_t numa_context_tracked_count(const NumaReplicationContext *ctx);

// Install the system topology / a bare single-node topology. `hardware` cannot differ
// from `system` here: both read /sys, and the BundledL3 split upstream applies for
// `hardware` is unported. Left aliased explicitly rather than silently.
void numa_context_set_system(NumaReplicationContext *ctx);
void numa_context_set_hardware(NumaReplicationContext *ctx);
void numa_context_set_none(NumaReplicationContext *ctx);

// Install an explicit topology string. Return false and leave the context's config in
// place when the string does not parse.
bool numa_context_set_from_string(NumaReplicationContext *ctx, const char *s, size_t len);

size_t numa_context_node_count(const NumaReplicationContext *ctx);
size_t numa_context_cpus_in_node(const NumaReplicationContext *ctx, size_t node);
bool numa_suggests_binding_threads(const NumaReplicationContext *ctx, size_t num_threads);

// Fill OUT_NODES with a node index per requested thread and return the node count in use
// (at least 1). Degrade to node 0 for every thread on OOM rather than aborting a search.
size_t numa_distribute_threads_among_nodes(const NumaReplicationContext *ctx,
                                           size_t requested,
                                           size_t *out_nodes);

// Bind the calling thread to NODE, run CALLBACK, and leave the binding in place. Upstream
// uses this to construct each Worker on the node that will own it, so the allocation and
// its first-touch pages land together.
void numa_execute_on_node(const NumaReplicationContext *ctx,
                          size_t node,
                          void (*callback)(void *ctx),
                          void *callback_ctx);

#endif  // MCFISH_NUMA_H
