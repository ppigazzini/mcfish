// The legal form of enum-cross.refuse.c: a Square reaches a Square.
#include "engine/board/types.h"
static Square take_square(Square s) { return s; }
Square right(void);
Square right(void) { return take_square(SQ_A1); }
