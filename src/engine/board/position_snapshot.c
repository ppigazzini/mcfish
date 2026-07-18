#include "position_snapshot.h"

#include "legality.h"
#include "position_query.h"

void snapshot_fill(const Position *pos, PositionSnapshot *out) { pos_fill_snapshot(pos, out); }

bool snapshot_move_is_legal(const Position *pos, Move m) { return pos_legal(pos, m); }
