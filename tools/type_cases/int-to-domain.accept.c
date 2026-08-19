// The legal form: the fold is named, and an explicit cast stays a line a reviewer
// can see -- which is why none of the promotions stops one.
#include "platform/syzygy/encode.h"
TbFile right(unsigned board_file);
TbFile right(unsigned board_file) { return tb_file_of_board_file(board_file); }
