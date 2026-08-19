// "A history bonus and the table's gravity clamp, transposed." HistLimit converts to
// nothing at all, so BOTH directions are a hard error -- this is the swap.
#include "engine/search/history.h"
void wrong(SharedStat *e, int bonus);
void wrong(SharedStat *e, int bonus) { shared_stats_update(e, HIST_LIMIT_MAIN, bonus); }
