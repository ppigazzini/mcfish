// A Direction reaching a Square. docs/09-type-design.md: "One domain enum reaching
// another's parameter -- a `Direction` where a `Square` belongs, and the reverse."
// REQUIRES: -Werror=enum-conversion
#include "engine/board/types.h"
static Square take_square(Square s) { return s; }
Square wrong(void);
Square wrong(void) {
    Direction d = NORTH;
    return take_square(d);
}
