// The other half of the same claim: the bare literal the call sites used to carry.
#include "engine/search/history.h"
void wrong(SharedStat *e, int bonus);
void wrong(SharedStat *e, int bonus) { shared_stats_update(e, bonus, 7183); }
