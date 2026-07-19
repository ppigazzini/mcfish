// Split the transposition table's storage types out of its implementation: the cluster
// the table is an array of, and the table handle a Worker holds a reference to.
//
// This header OWNS no memory and no probe logic -- `tt.c` keeps both. It exists so the
// state zone can type a worker's `tt` reference without pulling in the probe path, and so
// the cluster footprint is asserted in one place. The invariant is that a cluster is
// exactly 32 bytes: the entries plus two bytes of explicit padding, so a cluster never
// straddles a cache line in a 64-byte-aligned table.
//
// Upstream: tt.h:60 (TTEntry), tt.cpp:29 (Cluster, ClusterSize), tt.h:104
// (TranspositionTable). Port source: zfish src/engine/state/tt_types.zig.

#ifndef MCFISH_TT_TYPES_H
#define MCFISH_TT_TYPES_H

#include "../search/tt.h"

#include <stddef.h>
#include <stdint.h>

enum { TT_CLUSTER_SIZE = 3 };

typedef struct {
    TTEntry entry[TT_CLUSTER_SIZE];
    uint8_t padding[2];
} TTCluster;

static_assert(sizeof(TTEntry) == 10, "TTEntry must stay a packed 10-byte record");
static_assert(sizeof(TTCluster) == 32, "TTCluster must fill exactly half a cache line");

// Hold the table handle a Worker binds a reference to: the cluster count, the cluster
// base, and the running generation the store path ages entries by. The allocation and
// every probe stay in tt.c; this is the shape the reference has.
typedef struct {
    size_t cluster_count;
    TTCluster *table;
    uint8_t generation8;
} TranspositionTable;

#endif  // MCFISH_TT_TYPES_H
