// The legal form: the result is read, which is the whole point of the annotation.
#include "engine/board/position.h"
bool right(Position *pos, const char *fen, StateInfo *si);
bool right(Position *pos, const char *fen, StateInfo *si) { return pos_set(pos, fen, false, si); }
