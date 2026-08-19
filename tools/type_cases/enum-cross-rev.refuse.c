// "and the reverse": a Square reaching a Direction.
// REQUIRES: -Werror=enum-conversion
#include "engine/board/types.h"
static Direction take_direction(Direction d) { return d; }
Direction wrong(void);
Direction wrong(void) {
    Square s = SQ_A1;
    return take_direction(s);
}
