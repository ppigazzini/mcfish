// The legal form: bonus first, then the table's own named constant.
#include "engine/search/history.h"
void right(SharedStat *e, int bonus);
void right(SharedStat *e, int bonus) { shared_stats_update(e, bonus, HIST_LIMIT_MAIN); }
