// The legal form: a Direction reaches a Direction.
#include "engine/board/types.h"
static Direction take_direction(Direction d) { return d; }
Direction right(void);
Direction right(void) { return take_direction(SOUTH); }
