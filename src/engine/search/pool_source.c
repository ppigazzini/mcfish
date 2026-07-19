#include "pool_source.h"

// Leave every hook null: a caller that installs no pool reads its own worker's counters,
// which is the correct answer for a single-worker search and the only answer available
// to the headless callers (the unit tests, the bench harness).
PoolSource PoolCounters = { nullptr, nullptr, nullptr, nullptr };
