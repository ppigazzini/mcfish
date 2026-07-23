// Forward the transposition table's storage types. The cluster, the table handle and
// their footprint asserts moved into `../search/tt.h` alongside the inline probe --
// the probe needs the handle's fields visible to inline into the node bodies, and
// splitting the types from the probe would cycle the two headers. This header stays
// so a state-zone file can keep naming its tt reference without reaching across
// zones itself.
//
// Upstream: tt.cpp:62 (TTEntry), tt.cpp:154 (ClusterSize), tt.cpp:156 (Cluster),
// tt.h:79 (TranspositionTable).

#ifndef MCFISH_TT_TYPES_H
#define MCFISH_TT_TYPES_H

#include "../search/tt.h"

#endif  // MCFISH_TT_TYPES_H
